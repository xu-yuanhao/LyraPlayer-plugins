package org.lyraplayer.lyricon;

import android.app.Application;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Bundle;
import android.os.IBinder;
import android.os.RemoteException;
import android.os.SharedMemory;
import android.os.SystemClock;
import io.github.proify.lyricon.provider.IProviderBinder;
import io.github.proify.lyricon.provider.IProviderService;
import io.github.proify.lyricon.provider.IRemotePlayer;
import io.github.proify.lyricon.provider.IRemoteService;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ScheduledExecutorService;
import java.util.zip.DeflaterOutputStream;
import java.io.ByteArrayOutputStream;
import org.json.JSONArray;
import org.json.JSONObject;

/** Apache-2.0 protocol adaptation of Lyricon's Provider Bridge. See UPSTREAM.md. */
final class Protocol {
    static final String REGISTER="io.github.proify.lyricon.lyric.bridge.REGISTER_PROVIDER";
    static final String BOOT="io.github.proify.lyricon.lyric.bridge.CENTRAL_BOOT_COMPLETED";
    private final Context context;
    private final ScheduledExecutorService worker;
    private final byte[] info;
    private boolean closed,receiverRegistered;
    private volatile boolean active;
    private IRemoteService remote;
    private IRemotePlayer player;
    private SharedMemory memory;
    private ByteBuffer position;
    private IProviderBinder pending;
    private long pendingUntil,nextAttempt,backoff=1000,seek=-1;
    private Document sent;
    private boolean songSent;
    private Boolean playing;
    private IBinder.DeathRecipient death;
    private final BroadcastReceiver boot=new BroadcastReceiver(){public void onReceive(Context c,Intent i){enqueue(()->{if(!active){pending=null;nextAttempt=0;backoff=1000;}});}};

    Protocol(Context c,ScheduledExecutorService w){
        context=c;worker=w;
        try{
            JSONObject fields=new JSONObject();String name=c.getPackageName();
            fields.put("providerPackageName",name).put("playerPackageName",name);
            // Identity is the actual host package/process, never impersonate an app.
            if(Build.VERSION.SDK_INT>=28)fields.put("processName",Application.getProcessName());
            else fields.put("processName",name);
            info=fields.toString().getBytes(StandardCharsets.UTF_8);
        }catch(Exception failure){throw new IllegalStateException(failure);}
        IntentFilter filter=new IntentFilter(BOOT);
        if(Build.VERSION.SDK_INT>=33)c.registerReceiver(boot,filter,Context.RECEIVER_EXPORTED);
        else c.registerReceiver(boot,filter);
        receiverRegistered=true;
    }
    boolean connected(){return active;}
    private void enqueue(Runnable action){if(!worker.isShutdown())try{worker.execute(action);}catch(java.util.concurrent.RejectedExecutionException ignored){}}
    private void register(long now){
        if(pending!=null&&now<pendingUntil)return;
        if(now<nextAttempt)return;
        pendingUntil=now+4000;nextAttempt=pendingUntil+backoff;backoff=Math.min(30000,backoff*2);
        IProviderBinder attempt=new IProviderBinder.Stub(){
            public byte[] getProviderInfo(){return info.clone();}
            public IProviderService getProviderService(){return commands;}
            public void onRegistrationCallback(IRemoteService service){
                enqueue(()->{if(!closed&&pending==this&&SystemClock.elapsedRealtime()<=pendingUntil){pending=null;attach(service);}});
            }
        };
        pending=attempt;
        Bundle bundle=new Bundle();bundle.putBinder("binder",attempt.asBinder());
        context.sendBroadcast(new Intent(REGISTER).setPackage("com.android.systemui").putExtra("bundle",bundle));
    }
    // Optional provider commands are deliberately empty: transport controls stay
    // with Android MediaSession and the host's existing playback engine.
    private final IProviderService commands=new IProviderService.Stub(){public Bundle onRunCommand(Intent intent){return null;}};
    private void attach(IRemoteService service){
        disconnected();if(service==null)return;
        try{
            if(!service.asBinder().isBinderAlive())return;
            IBinder bound=service.asBinder();
            IBinder.DeathRecipient recipient=()->enqueue(()->{if(remote!=null&&remote.asBinder()==bound)disconnected();});
            bound.linkToDeath(recipient,0);remote=service;death=recipient;
            player=service.getPlayer();if(player==null)throw new RemoteException("missing player");
            memory=player.getPositionMemory();
            if(memory==null)throw new IllegalStateException("missing position memory");
            position=memory.mapReadWrite();if(position.capacity()<8)throw new IllegalStateException("short position memory");
            if(!service.asBinder().isBinderAlive())throw new RemoteException("dead service");
            active=true;backoff=1000;nextAttempt=0;
            player.setDisplayTranslation(true);player.setDisplayRoma(true);
        }catch(Exception failure){disconnected();}
    }
    void update(Document doc,Endpoint.State state)throws Exception {
        if(closed)return;
        if(!active){register(SystemClock.elapsedRealtime());return;}
        Document value=doc!=null&&doc.track==state.track&&!doc.lines.isEmpty()?doc:null;
        if(!songSent||value!=sent){
            player.setSong(value==null?null:song(value));sent=value;songSent=true;playing=null;seek=-1;
        }
        if(position!=null)position.putLong(0,state.position);
        boolean valid=state.playing&&value!=null;
        if(playing==null||playing!=valid){player.setPlaybackState(valid);playing=valid;}
        if(seek!=state.seek){player.seekTo(state.position);seek=state.seek;}
    }

    static byte[] song(Document doc)throws Exception {
        JSONObject song=new JSONObject().put("id",Long.toString(doc.track)).put("name",doc.title).put("artist",doc.artist).put("duration",doc.duration);
        JSONArray lines=new JSONArray();
        for(Document.Line l:doc.lines){
            JSONObject line=new JSONObject().put("begin",l.start).put("end",l.end).put("duration",l.end-l.start).put("text",l.text);
            if(!l.translation.isEmpty())line.put("translation",l.translation);
            if(!l.phonetic.isEmpty())line.put("roma",l.phonetic);
            JSONArray words=new JSONArray();for(Document.Word w:l.words)words.put(new JSONObject().put("begin",w.start).put("end",w.end).put("duration",w.end-w.start).put("text",w.text));
            if(words.length()>0)line.put("words",words);lines.put(line);
        }
        song.put("lyrics",lines);
        byte[] bytes=song.toString().getBytes(StandardCharsets.UTF_8);
        if(bytes.length>4*1024*1024)throw new IllegalArgumentException("song too large");
        ByteArrayOutputStream out=new ByteArrayOutputStream();
        try(DeflaterOutputStream zipped=new DeflaterOutputStream(out)){zipped.write(bytes);}
        byte[] zipped=out.toByteArray();
        if(zipped.length>384*1024)throw new IllegalArgumentException("Binder payload too large");
        return zipped;
    }
    void disconnected(){
        active=false;pending=null;sent=null;songSent=false;playing=null;seek=-1;
        if(remote!=null&&death!=null)remote.asBinder().unlinkToDeath(death,0);
        death=null;
        if(position!=null){SharedMemory.unmap(position);position=null;}
        if(memory!=null){memory.close();memory=null;}
        player=null;remote=null;
        nextAttempt=SystemClock.elapsedRealtime()+backoff;backoff=Math.min(30000,backoff*2);
    }
    void close(){
        if(closed)return;closed=true;pending=null;
        if(receiverRegistered){try{context.unregisterReceiver(boot);}catch(RuntimeException ignored){}receiverRegistered=false;}
        try{if(player!=null){player.setPlaybackState(false);player.setSong(null);}if(remote!=null)remote.disconnect();}catch(Exception ignored){}
        disconnected();
    }
}
