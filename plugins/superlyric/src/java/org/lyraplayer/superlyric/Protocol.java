package org.lyraplayer.superlyric;

import android.content.Context;
import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.os.SystemClock;
import java.lang.reflect.Method;
import java.util.concurrent.ScheduledExecutorService;

/** SuperLyric 4.x published AIDL protocol, independent of the rendering host. */
final class Protocol {
    static final String DESCRIPTOR="com.hchen.superlyricapi.v4.ISuperLyricService";
    interface Locator { IBinder find() throws Exception; }
    private final Locator locator;
    private volatile IBinder service;
    private Document sent;
    private boolean closed;
    private long retry,publishAfter;
    Protocol(Context context,ScheduledExecutorService worker) {
        this(()->{
            // Upstream exposes its Binder through ServiceManager, with no
            // equivalent public SDK lookup API. Never bypass hidden API policy.
            Class<?> manager=Class.forName("android.os.ServiceManager");
            Method get=manager.getDeclaredMethod("getService",String.class);
            return (IBinder)get.invoke(null,"super_lyric.v4");
        });
    }
    Protocol(Locator source){locator=source;}
    boolean connected(){IBinder current=service;return current!=null&&current.isBinderAlive();}
    void update(Document doc,Endpoint.State state)throws Exception {
        if(closed)return;
        long now=SystemClock.elapsedRealtime();
        if(!connected()){
            service=null;sent=null;
            if(now<retry)return;
            retry=now+5000;service=locator.find();
            if(service==null)return;
        }
        Document current=doc!=null&&doc.track==state.track&&!doc.lines.isEmpty()&&!doc.title.trim().isEmpty()?doc:null;
        if(current==sent)return;
        if(current==null){clear();sent=null;return;}
        if(now<publishAfter)return;
        byte[] frame=FrameEncoder.full(current);
        Parcel data=Parcel.obtain(),reply=Parcel.obtain();
        try{
            data.writeInterfaceToken(DESCRIPTOR);data.writeByteArray(frame);
            if(!service.transact(IBinder.FIRST_CALL_TRANSACTION,data,reply,0))throw new RemoteException("publish unsupported");
            reply.readException();int result=reply.readInt();
            if(result==0){sent=current;publishAfter=0;}
            else {publishAfter=now+5000;}
        }finally{reply.recycle();data.recycle();}
    }
    private void clear()throws RemoteException{
        Parcel data=Parcel.obtain(),reply=Parcel.obtain();
        try{data.writeInterfaceToken(DESCRIPTOR);if(service.transact(IBinder.FIRST_CALL_TRANSACTION+1,data,reply,0))reply.readException();}
        finally{reply.recycle();data.recycle();}
    }
    void disconnected(){service=null;sent=null;publishAfter=0;retry=SystemClock.elapsedRealtime()+5000;}
    void close(){if(closed)return;closed=true;try{if(connected()&&sent!=null)clear();}catch(Exception ignored){}service=null;sent=null;}
}
