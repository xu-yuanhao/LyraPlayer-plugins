package org.lyraplayer.superlyric;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.List;

/** SuperLyricApi 4.0 documented big-endian tagged fields. */
final class FrameEncoder {
    interface Body {void write(DataOutputStream out)throws IOException;}
    static byte[] bytes(Body body)throws IOException {ByteArrayOutputStream b=new ByteArrayOutputStream();body.write(new DataOutputStream(b));if(b.size()>512*1024)throw new IOException("SuperLyric frame too large");return b.toByteArray();}
    static void variable(DataOutputStream out,int id,int type,byte[] body)throws IOException{out.writeInt(id);out.writeByte(type);out.writeInt(body.length);out.write(body);}
    static byte[] string(String text)throws IOException{return bytes(out->{byte[] b=text.getBytes(StandardCharsets.UTF_8);out.writeInt(b.length);out.write(b);});}
    static void text(DataOutputStream out,int id,String value)throws IOException{variable(out,id,6,string(value));}
    static void number(DataOutputStream out,int id,long value)throws IOException{out.writeInt(id);out.writeByte(3);out.writeLong(value);}
    static void element(DataOutputStream out,byte[] struct)throws IOException{out.writeByte(9);out.writeInt(struct.length);out.write(struct);}
    static byte[] word(Document.Word w)throws IOException{return bytes(out->{out.writeInt(3);text(out,1,w.text);number(out,2,w.start);number(out,3,w.end);});}
    static byte[] line(Document.Line l)throws IOException{return bytes(out->{out.writeInt(6);text(out,1,l.text);number(out,2,l.start);number(out,3,l.end);variable(out,4,8,bytes(list->{list.writeInt(l.words.size());for(Document.Word w:l.words)element(list,word(w));}));text(out,5,l.translation);text(out,6,l.phonetic);});}
    static byte[] full(Document d)throws IOException{return bytes(out->{out.writeInt(0x534c5904);out.writeInt(1);out.writeInt(2);variable(out,1,10,string("FULL"));variable(out,2,9,bytes(doc->{doc.writeInt(6);text(doc,1,Long.toString(d.track));text(doc,2,d.title);text(doc,3,d.artist);text(doc,4,d.album);number(doc,5,d.duration);variable(doc,6,8,bytes(lines->{lines.writeInt(d.lines.size());for(Document.Line l:d.lines)element(lines,line(l));}));}));});}
}
