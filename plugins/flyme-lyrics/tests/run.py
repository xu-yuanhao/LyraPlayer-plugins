#!/usr/bin/env python3
"""Execute the production notification bridge with deterministic SDK doubles."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
files={
'android/content/Context.java': '''package android.content; public class Context {public android.app.NotificationManager manager=new android.app.NotificationManager(); public Context getApplicationContext(){return this;} public <T> T getSystemService(Class<T> k){return k.cast(manager);}}''',
'android/text/TextUtils.java': '''package android.text; public class TextUtils {public static boolean equals(CharSequence a,CharSequence b){return a==null?b==null:a.toString().contentEquals(b==null?"":b);}}''',
'android/app/Notification.java': '''package android.app; public class Notification {
 public static final int FLAG_ALWAYS_SHOW_TICKER=Boolean.getBoolean("unsupported")?0:1024,FLAG_ONLY_UPDATE_TICKER=2048,FLAG_NO_CLEAR=32,FLAG_ONLY_ALERT_ONCE=8;
 public static final String CATEGORY_TRANSPORT="transport", EXTRA_MEDIA_SESSION="session";
 public String category="transport", title="song", controls="previous,pause,next", cover="art";public CharSequence tickerText;public int flags=64;public java.util.Map<String,Object> extras=new java.util.HashMap<>();
 public Notification(){extras.put(EXTRA_MEDIA_SESSION,"session-token");}
 public static class Builder {Notification n;public static Builder recoverBuilder(android.content.Context c,Notification old){Builder b=new Builder();b.n=new Notification();b.n.category=old.category;b.n.title=old.title;b.n.controls=old.controls;b.n.cover=old.cover;b.n.tickerText=old.tickerText;b.n.flags=old.flags;b.n.extras=new java.util.HashMap<>(old.extras);return b;}public Builder setTicker(CharSequence s){n.tickerText=s;return this;}public Builder setOnlyAlertOnce(boolean v){if(v)n.flags|=8;return this;}public Notification build(){return n;}}
}''',
'android/service/notification/StatusBarNotification.java': '''package android.service.notification; public class StatusBarNotification {public android.app.Notification n;int id;String tag; public StatusBarNotification(String tag,int id,android.app.Notification n){this.tag=tag;this.id=id;this.n=n;}public android.app.Notification getNotification(){return n;}public String getKey(){return tag+":"+id;}public int getId(){return id;}public String getTag(){return tag;}}''',
'android/app/NotificationManager.java': '''package android.app; import android.service.notification.StatusBarNotification;public class NotificationManager {public java.util.Map<String,StatusBarNotification> items=new java.util.HashMap<>();public int writes;public boolean fail;public StatusBarNotification[] getActiveNotifications(){if(fail)throw new SecurityException();return items.values().toArray(new StatusBarNotification[0]);}public void notify(String tag,int id,Notification n){writes++;items.put(tag+":"+id,new StatusBarNotification(tag,id,n));}}''',
'org/lyraplayer/flymelyrics/BridgeTest.java': '''package org.lyraplayer.flymelyrics;
import android.app.*;import android.content.*;import android.service.notification.*;import java.nio.charset.StandardCharsets;
public class BridgeTest {
 static void check(boolean value,String reason){if(!value)throw new AssertionError(reason);}
 static void cue(NotificationBridge b,String text){b.setCue(text.getBytes(StandardCharsets.UTF_8));b.refresh();}
 public static void main(String[] args){
 Context c=new Context();Notification n=new Notification();c.manager.items.put("music:7",new StatusBarNotification("music",7,n));
 Notification unrelated=new Notification();unrelated.category="message";c.manager.items.put("msg:8",new StatusBarNotification("msg",8,unrelated));
 NotificationBridge b=new NotificationBridge(c);
 synchronized(b){
 if(Boolean.getBoolean("unsupported")){check(!b.supported(),"capability detection");cue(b,"不应显示");check(c.manager.writes==0,"unsupported device is inert");b.stop();return;}
 check(b.supported(),"documented Flyme flags");cue(b,"第一句 🎵");Notification shown=c.manager.items.get("music:7").n;
 check("第一句 🎵".contentEquals(shown.tickerText),"UTF-8 emoji");check((shown.flags&3072)==3072,"both vendor flags");check((shown.flags&32)!=0,"persistent ticker");
 check(shown.title.equals(n.title)&&shown.controls.equals(n.controls)&&shown.cover.equals(n.cover)&&shown.extras.equals(n.extras),"preserve media controls/art/token");
 check(c.manager.items.get("msg:8").n==unrelated,"only media notification");int writes=c.manager.writes;b.refresh();check(c.manager.writes==writes,"no duplicate notifications");
 Notification rebuilt=new Notification();rebuilt.title="new art and title";c.manager.items.get("music:7").n=rebuilt;b.refresh();check(c.manager.items.get("music:7").n.title.equals(rebuilt.title),"reapply after host rebuild without losing metadata");
 cue(b,"");Notification cleared=c.manager.items.get("music:7").n;check(cleared.tickerText==null&&(cleared.flags&3104)==0,"pause/gap clear and restore no-clear");
 cue(b,"jump destination");check("jump destination".contentEquals(c.manager.items.get("music:7").n.tickerText),"seek cue changes");
 c.manager.fail=true;b.refresh();c.manager.fail=false;cue(b,"recover");check("recover".contentEquals(c.manager.items.get("music:7").n.tickerText),"recover after notification permission error");
 c.manager.items.remove("music:7");b.refresh();check(!c.manager.items.containsKey("music:7"),"never resurrect removed notification");
 n=new Notification();n.flags|=32;c.manager.items.put("music:7",new StatusBarNotification("music",7,n));cue(b,"last");b.stop();Notification stopped=c.manager.items.get("music:7").n;
 check(stopped.tickerText==null&&(stopped.flags&3072)==0&&(stopped.flags&32)!=0,"unload clears ticker and preserves original no-clear");writes=c.manager.writes;cue(b,"late");check(c.manager.writes==writes,"no callbacks after stop");b.stop();
 }
 System.out.println("Notification bridge lifecycle, recovery and UTF-8 passed");
 }}''',
}
with tempfile.TemporaryDirectory() as d:
    temp=Path(d)
    for name,text in files.items():
        p=temp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    production=root/'src/java/org/lyraplayer/flymelyrics/NotificationBridge.java'
    subprocess.run(['javac','--release','8','-d',d,*map(str,temp.rglob('*.java')),str(production)],check=True)
    for flag in ('false','true'):
        subprocess.run(['java','-Dunsupported='+flag,'-cp',d,'org.lyraplayer.flymelyrics.BridgeTest'],check=True)
    p=temp/'timeline.cpp';p.write_text('''#include "timeline.h"
#include <cassert>
#include <cstring>
int main(){std::vector<flyme::Cue> c={{100,300,"lead"},{200,250,"duet"},{400,500,"next"}};
assert(!*flyme::current(c,0));assert(!strcmp(flyme::current(c,100),"lead"));assert(!strcmp(flyme::current(c,220),"duet"));assert(!strcmp(flyme::current(c,260),"lead"));assert(!*flyme::current(c,300));assert(!strcmp(flyme::current(c,450),"next"));assert(!strcmp(flyme::current(c,110),"lead"));assert(!*flyme::current({},450));}''')
    subprocess.run(['c++','-std=c++17','-I',str(root/'src'),str(p),'-o',str(temp/'timeline')],check=True)
    subprocess.run([str(temp/'timeline')],check=True)
print('Timeline gaps, overlap, seek and empty-track tests passed')
