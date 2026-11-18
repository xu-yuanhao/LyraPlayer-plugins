package org.lyraplayer.flymelyrics;
import android.content.Context;
public class DeviceProbe {
 private static native int check(Context context,String path);
 public static void main(String[] args)throws Exception {
  Context c=new android.content.ContextWrapper(null) {
   public Context getApplicationContext(){return this;}
   public ClassLoader getClassLoader(){return DeviceProbe.class.getClassLoader();}
   public String getSystemServiceName(Class<?> service){return "notification";}
   public Object getSystemService(String name){return null;}
  };
  try {
   android.app.Notification.class.getField("FLAG_ALWAYS_SHOW_TICKER");
   android.app.Notification.class.getField("FLAG_ONLY_UPDATE_TICKER");
   System.out.println("Flyme ticker fields available");
  } catch(ReflectiveOperationException error){System.out.println("Flyme ticker fields absent");}
  System.load(args[0]);int result=check(c,args[1]);
  if(result!=0)throw new AssertionError("Plugin lifecycle: "+result);
  System.out.println("Embedded DEX plugin init/shutdown passed");
 }
}
