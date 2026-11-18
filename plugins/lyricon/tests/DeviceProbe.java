package org.lyraplayer.lyricontest;
import android.content.*;
import android.os.*;
import android.media.session.PlaybackState;
import io.github.proify.lyricon.provider.*;
import org.json.*;
import java.io.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.zip.InflaterInputStream;

/** Loads the actual release .so/embedded DEX and exercises generated Binder proxies. */
public class DeviceProbe {
 private static native long open(Context c,String path);
 private static native void step(long pointer,int phase);
 private static native void close(long pointer);
 static IBinder proxy(IBinder target){return new Binder(){protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{return target.transact(code,data,reply,flags);}};}
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static class Player extends IRemotePlayer.Stub {
  final SharedMemory memory;final ByteBuffer view;volatile JSONObject song;volatile long seek=-1;volatile boolean playing,throwOnce;final AtomicInteger songs=new AtomicInteger();
  Player()throws Exception{memory=SharedMemory.create("LyraLyriconTest",8);view=memory.mapReadWrite();}
  public void setSong(byte[] bytes)throws RemoteException{
   if(throwOnce){throwOnce=false;throw new RemoteException("test disconnect");}
   try{if(bytes==null){song=null;}else{ByteArrayOutputStream out=new ByteArrayOutputStream();try(InflaterInputStream in=new InflaterInputStream(new ByteArrayInputStream(bytes))){byte[] b=new byte[1024];int n;while((n=in.read(b))!=-1)out.write(b,0,n);}song=new JSONObject(new String(out.toByteArray(),StandardCharsets.UTF_8));}songs.incrementAndGet();}catch(Exception e){throw new IllegalArgumentException(e);}
  }
  public void setPlaybackState(boolean value){playing=value;}
  public void seekTo(long value){seek=value;}
  public void sendText(String text){throw new AssertionError("must send structured timeline");}
  public void setPositionUpdateInterval(int interval){}
  public void setDisplayTranslation(boolean value){check(value,"translation enabled");}
  public SharedMemory getPositionMemory(){return memory;}
  public void setDisplayRoma(boolean value){check(value,"phonetic enabled");}
  public void setPlaybackState2(PlaybackState state){throw new AssertionError("must use one shared-memory position implementation");}
  void dispose(){SharedMemory.unmap(view);memory.close();}
 }
 static class Central extends IRemoteService.Stub {
  final Player player;volatile boolean disconnected;final AtomicInteger gets=new AtomicInteger();
  Central(Player p){player=p;}
  public IRemotePlayer getPlayer(){gets.incrementAndGet();return IRemotePlayer.Stub.asInterface(proxy(player.asBinder()));}
  public void disconnect(){disconnected=true;}
 }
 static class TestContext extends ContextWrapper {
  volatile BroadcastReceiver receiver;volatile IProviderBinder registration;volatile Central central;final AtomicInteger attempts=new AtomicInteger();
  TestContext(){super(null);}
  public Context getApplicationContext(){return this;}
  public ClassLoader getClassLoader(){return DeviceProbe.class.getClassLoader();}
  public String getPackageName(){return "com.petehsu.lyraplayer";}
  public Intent registerReceiver(BroadcastReceiver r,IntentFilter f){receiver=r;return null;}
  public Intent registerReceiver(BroadcastReceiver r,IntentFilter f,int flags){receiver=r;return null;}
  public void unregisterReceiver(BroadcastReceiver r){check(receiver==r,"receiver identity");receiver=null;}
  public void sendBroadcast(Intent i){
   check("com.android.systemui".equals(i.getPackage()),"explicit central package");
   try{
    IProviderBinder p=IProviderBinder.Stub.asInterface(proxy(i.getBundleExtra("bundle").getBinder("binder")));
    JSONObject info=new JSONObject(new String(p.getProviderInfo(),StandardCharsets.UTF_8));
    check(getPackageName().equals(info.getString("providerPackageName")),"real host identity");
    check(p.getProviderService().onRunCommand(null)==null,"no private playback commands");
    registration=p;attempts.incrementAndGet();Central c=central;
    if(c!=null)p.onRegistrationCallback(IRemoteService.Stub.asInterface(proxy(c.asBinder())));
   }catch(Exception e){throw new IllegalStateException(e);}
  }
 }
 interface Condition{boolean ready()throws Exception;}
 static void await(long handle,int phase,Condition condition)throws Exception{
  long until=SystemClock.elapsedRealtime()+6000;
  while(!condition.ready()&&SystemClock.elapsedRealtime()<until)step(handle,phase);
  check(condition.ready(),"timeout phase "+phase);
 }
 public static void main(String[] args)throws Exception{
  System.load(args[0]);TestContext context=new TestContext();Player player=new Player();context.central=new Central(player);
  long h=open(context,args[1]);check(h!=0,"plugin ABI and embedded DEX startup");
  try{
   await(h,0,()->player.song!=null&&player.playing&&player.view.getLong(0)==1250);
   JSONObject song=player.song;check(song.getString("name").equals("first"),"metadata");
   JSONArray lines=song.getJSONArray("lyrics");check(lines.length()==2,"complete timeline");
   JSONObject line=lines.getJSONObject(0);check(line.getString("text").equals("你好 🎵"),"UTF-8 supplementary text");
   check(line.getString("translation").equals("translation")&&line.getString("roma").equals("ni hao"),"translation and phonetic");
   check(line.getJSONArray("words").length()==2&&line.getJSONArray("words").getJSONObject(1).getLong("begin")==1000,"absolute word timestamps");
   int sent=player.songs.get();for(int i=0;i<5;i++)step(h,0);check(player.songs.get()==sent,"no per-frame document republication");
   await(h,1,()->!player.playing);await(h,2,()->player.playing&&player.seek==2500&&player.view.getLong(0)==2500);
   await(h,3,()->player.song!=null&&player.song.optString("name").equals("second"));
   await(h,4,()->player.song==null&&!player.playing);
   Player replacement=new Player();Central old=context.central;context.central=new Central(replacement);
   // Simulate a central boot after the old player's next transaction fails.
   player.throwOnce=true;for(int i=0;i<8;i++)step(h,3);
   context.receiver.onReceive(context,new Intent("io.github.proify.lyricon.lyric.bridge.CENTRAL_BOOT_COMPLETED"));
   await(h,3,()->replacement.song!=null&&replacement.playing);
   check(context.attempts.get()>=2,"central re-registration");
   IProviderBinder late=context.registration;Central active=context.central;
   close(h);h=0;long until=SystemClock.elapsedRealtime()+2000;while(context.receiver!=null&&SystemClock.elapsedRealtime()<until)Thread.sleep(10);
   check(context.receiver==null&&active.disconnected&&!replacement.playing&&replacement.song==null,"unload clears remote and receiver");
   int gets=old.gets.get();late.onRegistrationCallback(IRemoteService.Stub.asInterface(proxy(old.asBinder())));Thread.sleep(50);check(old.gets.get()==gets,"late callback must not attach an old service");
   replacement.dispose();
  }finally{if(h!=0)close(h);player.dispose();}
  System.out.println("Lyricon release plugin: native ABI -> embedded DEX -> AIDL -> JSON/ZLIB/SharedMemory passed; pause/seek/switch/reconnect/unload passed");
 }
}
