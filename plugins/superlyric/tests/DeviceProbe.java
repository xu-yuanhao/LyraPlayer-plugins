package org.lyraplayer.superlyrictest;
import android.content.*;
import android.os.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.lang.reflect.*;
import java.util.*;
import java.util.concurrent.atomic.AtomicInteger;
public class DeviceProbe {
 private static native long open(Context c,String path);
 private static native void step(long pointer,int phase);
 private static native void close(long pointer);
 static void check(boolean ok,String why){if(!ok)throw new AssertionError(why);}
 static Object value(DataInputStream in,int type)throws Exception{
  if(type==0)return null;if(type==3)return in.readLong();
  int size=in.readInt();check(size>=0&&size<=in.available(),"field bounds");byte[] bytes=new byte[size];in.readFully(bytes);DataInputStream body=new DataInputStream(new ByteArrayInputStream(bytes));Object result;
  if(type==6||type==10){int n=body.readInt();byte[] text=new byte[n];body.readFully(text);result=new String(text,StandardCharsets.UTF_8);}
  else if(type==9)result=struct(body);
  else if(type==8){int n=body.readInt();List<Object> list=new ArrayList<>();for(int i=0;i<n;i++)list.add(value(body,body.readUnsignedByte()));result=list;}
  else throw new AssertionError("type "+type);
  check(body.available()==0,"field fully consumed");return result;
 }
 static Map<Integer,Object> struct(DataInputStream in)throws Exception{int n=in.readInt();check(n>=0&&n<64,"field count");Map<Integer,Object> fields=new HashMap<>();for(int i=0;i<n;i++){int id=in.readInt();fields.put(id,value(in,in.readUnsignedByte()));}return fields;}
 static class Server extends Binder {
  volatile Map<Integer,Object> document;AtomicInteger published=new AtomicInteger(),cleared=new AtomicInteger();volatile int result;volatile boolean failOnce;
  protected boolean onTransact(int code,Parcel data,Parcel reply,int flags)throws RemoteException{
   data.enforceInterface("com.hchen.superlyricapi.v4.ISuperLyricService");
   if(code==1){if(failOnce){failOnce=false;throw new RemoteException("test failure");}try{byte[] bytes=data.createByteArray();check(bytes.length<=512*1024,"upstream frame limit");DataInputStream in=new DataInputStream(new ByteArrayInputStream(bytes));check(in.readInt()==0x534c5904&&in.readInt()==1,"upstream magic/version");Map<Integer,Object> frame=struct(in);check(in.available()==0&&"FULL".equals(frame.get(1)),"full frame");document=(Map<Integer,Object>)frame.get(2);published.incrementAndGet();reply.writeNoException();reply.writeInt(result);return true;}catch(Exception e){throw new IllegalStateException(e);}}
   if(code==2){cleared.incrementAndGet();document=null;reply.writeNoException();return true;}return super.onTransact(code,data,reply,flags);
  }
 }
 static class TestContext extends ContextWrapper {
  TestContext(){super(null);}public Context getApplicationContext(){return this;}public ClassLoader getClassLoader(){return DeviceProbe.class.getClassLoader();}public String getPackageName(){return "com.petehsu.lyraplayer";}
 }
 interface Condition{boolean ready()throws Exception;}
 static void await(long h,int phase,Condition c)throws Exception{long end=SystemClock.elapsedRealtime()+8000;while(!c.ready()&&SystemClock.elapsedRealtime()<end)step(h,phase);check(c.ready(),"timeout phase "+phase);}
 public static void main(String[] args)throws Exception{
  // Runtime service injection exercises the actual ServiceManager lookup used
  // by the release plugin without installing or modifying any host application.
  Server server=new Server();Class<?> sm=Class.forName("android.os.ServiceManager");Field cache=sm.getDeclaredField("sCache");cache.setAccessible(true);Map<String,IBinder> services=(Map<String,IBinder>)cache.get(null);services.put("super_lyric.v4",server);
  System.load(args[0]);long h=open(new TestContext(),args[1]);check(h!=0,"actual plugin ABI/DEX init");
  try{
   await(h,0,()->server.document!=null);Map<Integer,Object> doc=server.document;
   check(doc.get(2).equals("first")&&doc.get(3).equals("artist"),"metadata");
   List<Map<Integer,Object>> lines=(List<Map<Integer,Object>>)doc.get(6);check(lines.size()==2,"complete timeline");Map<Integer,Object> line=lines.get(0);
   check(line.get(1).equals("你好 🎵")&&line.get(5).equals("translation")&&line.get(6).equals("ni hao"),"UTF-8 translation phonetic");
   List<Map<Integer,Object>> words=(List<Map<Integer,Object>>)line.get(4);check(words.size()==2&&words.get(1).get(2).equals(1000L),"word timestamps");
   int count=server.published.get();for(int i=0;i<24;i++)step(h,0);check(server.published.get()==count,"no repeated full documents");
   for(int i=0;i<24;i++)step(h,1);check(server.cleared.get()==0,"pause retains document for MediaSession");
   server.failOnce=true;await(h,3,()->server.document!=null&&server.document.get(2).equals("second"));
   await(h,4,()->server.cleared.get()>0);await(h,3,()->server.document!=null);
   close(h);h=0;long end=SystemClock.elapsedRealtime()+2000;while(server.document!=null&&SystemClock.elapsedRealtime()<end)Thread.sleep(20);check(server.document==null,"unload clears publisher");
  }finally{if(h!=0)close(h);services.remove("super_lyric.v4");}
  System.out.println("SuperLyric release plugin ABI/embedded DEX/Binder v4 frame, UTF-8, words, pause, switch, stop and unload passed");
 }
}
