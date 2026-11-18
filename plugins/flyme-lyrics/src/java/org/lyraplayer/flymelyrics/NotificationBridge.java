package org.lyraplayer.flymelyrics;

import android.app.Notification;
import android.app.NotificationManager;
import android.content.Context;
import android.service.notification.StatusBarNotification;
import android.text.TextUtils;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;

/** Only operates on this UID's existing media notification; never creates a second one. */
public final class NotificationBridge {
    private final Context context;
    private final NotificationManager manager;
    private final int mask;
    private final Map<String, Boolean> originals = new HashMap<>();
    private ScheduledExecutorService executor;
    private String cue = "";
    private boolean stopped;

    public NotificationBridge(Context source) {
        Context app = source.getApplicationContext();
        context = app != null ? app : source;
        manager = context.getSystemService(NotificationManager.class);
        int flags = 0;
        // Flyme's documented vendor extension, not present in the standard SDK.
        try {
            int show = Notification.class.getField("FLAG_ALWAYS_SHOW_TICKER").getInt(null);
            int update = Notification.class.getField("FLAG_ONLY_UPDATE_TICKER").getInt(null);
            if (show != 0 && update != 0 && (show & update) == 0) flags = show | update;
        } catch (ReflectiveOperationException | SecurityException ignored) { }
        mask = flags;
        if (supported()) {
            executor = Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "FlymeLyricsNotification");
                t.setDaemon(true);
                return t;
            });
            executor.scheduleWithFixedDelay(this::refresh, 0, 250, TimeUnit.MILLISECONDS);
        }
    }

    public boolean supported() { return mask != 0 && manager != null; }

    public synchronized void setCue(byte[] text) {
        if (stopped) return;
        String next = text == null ? "" : new String(text, StandardCharsets.UTF_8);
        if (!cue.equals(next)) {
            cue = next;
            if (executor != null) executor.execute(this::refresh);
        }
    }

    synchronized void refresh() {
        if (stopped || !supported()) return;
        try { apply(cue); } catch (RuntimeException ignored) {
            // Notification permission/channel changes must not terminate the scheduler.
        }
    }

    private void apply(String text) {
        Set<String> live = new HashSet<>();
        for (StatusBarNotification entry : manager.getActiveNotifications()) {
            Notification original = entry.getNotification();
            String key = entry.getKey();
            live.add(key);
            if (!Notification.CATEGORY_TRANSPORT.equals(original.category)
                    || !original.extras.containsKey(Notification.EXTRA_MEDIA_SESSION)) continue;
            boolean show = !text.isEmpty();
            boolean owned = originals.containsKey(key);
            if (!show && !owned) continue;
            if (show && !owned)
                originals.put(key, (original.flags & Notification.FLAG_NO_CLEAR) != 0);
            int expected = show ? original.flags | mask | Notification.FLAG_NO_CLEAR
                                : original.flags & ~mask;
            if (!show && !originals.get(key)) expected &= ~Notification.FLAG_NO_CLEAR;
            String ticker = show ? text : null;
            if (expected != original.flags || !TextUtils.equals(original.tickerText, ticker)) {
                Notification replacement = Notification.Builder.recoverBuilder(context, original)
                        .setTicker(ticker).setOnlyAlertOnce(true).build();
                replacement.flags = expected | Notification.FLAG_ONLY_ALERT_ONCE;
                manager.notify(entry.getTag(), entry.getId(), replacement);
            }
            if (!show) originals.remove(key);
        }
        originals.keySet().retainAll(live);
    }

    public synchronized void stop() {
        if (stopped) return;
        stopped = true;
        if (executor != null) { executor.shutdownNow(); executor = null; }
        if (supported()) {
            try { apply(""); } catch (RuntimeException ignored) { }
        }
        originals.clear();
        cue = "";
    }
}
