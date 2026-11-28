package org.lyraplayer.lyricon;

import android.content.Context;
import android.os.Build;
import java.io.IOException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/** Native only copies immutable data; all Binder work runs off the host looper. */
public final class Endpoint {
    static final class State {
        final long track,position,seek; final boolean playing;
        State(long t,long p,long s,boolean v){track=t;position=Math.max(0,p);seek=s;playing=v;}
    }
    private byte[] staged,pending;
    volatile State state=new State(-1,0,0,false);
    volatile boolean stopped;
    private final ScheduledExecutorService worker;
    private final Protocol protocol;
    private Document document;

    private final android.content.SharedPreferences preferences;

    public Endpoint(Context source) {
        Context app=source.getApplicationContext();Context context=app==null?source:app;
        preferences=context.getSharedPreferences("official.lyricon",Context.MODE_PRIVATE);
        if(Build.VERSION.SDK_INT<27){worker=null;protocol=null;return;}
        worker=Executors.newSingleThreadScheduledExecutor(r->{Thread t=new Thread(r,"LyraLyricon");t.setDaemon(true);return t;});
        protocol=new Protocol(context,worker);
        worker.scheduleWithFixedDelay(this::pump,0,50,TimeUnit.MILLISECONDS);
    }
    public int loadMode(){return preferences.getInt("display_mode",0);}
    public boolean saveMode(int mode){
        if(mode<0||mode>2)return false;
        preferences.edit().putInt("display_mode",mode).apply();
        return true;
    }
    public boolean supported(){return protocol!=null;}
    public synchronized void setDocument(byte[] bytes){if(!stopped)staged=bytes;}
    public synchronized int setState(long track,long position,long seek,boolean playing){
        if(stopped)return 1000;
        if(staged!=null){pending=staged;staged=null;}
        state=new State(track,position,seek,playing);
        return protocol!=null&&protocol.connected()?(playing?50:250):1000;
    }
    private void pump(){
        byte[] bytes;State snapshot;
        synchronized(this){if(stopped)return;bytes=pending;pending=null;snapshot=state;}
        if(bytes!=null){try{document=new Document(bytes);}catch(IOException bad){document=null;}}
        try{protocol.update(document,snapshot);}catch(Exception failure){protocol.disconnected();}
    }
    public synchronized void stop(){
        if(stopped)return;stopped=true;staged=pending=null;
        if(worker!=null){
            // Serialize cleanup after in-flight Binder calls. No worker touches
            // native code, borrowed host data or Activity objects after unload.
            worker.execute(()->{protocol.close();document=null;});
            worker.shutdown();
        }
    }
}
