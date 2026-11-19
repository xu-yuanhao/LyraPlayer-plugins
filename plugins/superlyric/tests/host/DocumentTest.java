package org.lyraplayer.superlyric;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
public class DocumentTest {
 static void text(DataOutputStream out,String value)throws Exception{byte[] b=value.getBytes(StandardCharsets.UTF_8);out.writeInt(b.length);out.write(b);}
 static byte[] fixture()throws Exception{
  ByteArrayOutputStream b=new ByteArrayOutputStream();DataOutputStream o=new DataOutputStream(b);
  o.writeInt(0x4c595231);o.writeLong(7);o.writeLong(4);o.writeLong(5000);text(o,"标题 🎵");text(o,"artist");text(o,"album");o.writeInt(1);
  o.writeLong(1000);o.writeLong(2000);text(o,"hello world 🎵");text(o,"译文");text(o,"ni hao");o.writeInt(2);
  o.writeLong(1000);o.writeLong(1500);text(o,"hello ");o.writeLong(1500);o.writeLong(2000);text(o,"world 🎵");return b.toByteArray();
 }
 static void reject(byte[] b)throws Exception{try{new Document(b);throw new AssertionError("accepted malformed document");}catch(IOException expected){}}
 public static void main(String[] args)throws Exception{
  byte[] b=fixture();Document d=new Document(b);
  if(d.track!=7||d.duration!=5000||!d.title.equals("标题 🎵")||!d.lines.get(0).words.get(0).text.equals("hello ")||!d.lines.get(0).phonetic.equals("ni hao"))throw new AssertionError("round trip");
  for(int n=0;n<b.length;n++)reject(Arrays.copyOf(b,n));
  byte[] extra=Arrays.copyOf(b,b.length+1);reject(extra);
  byte[] bad=b.clone();bad[0]=0;reject(bad);
  reject(new byte[4*1024*1024+1]);
  try{d.lines.clear();throw new AssertionError("mutable document");}catch(UnsupportedOperationException expected){}
  System.out.println("Document UTF-8, whitespace, word time, bounds and all truncations passed");
 }
}
