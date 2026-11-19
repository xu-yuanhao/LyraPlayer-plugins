package org.lyraplayer.superlyric;

import java.io.ByteArrayInputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/** Bounded private native/Java transfer; this is not the Lyricon wire protocol. */
final class Document {
    static final class Word {
        final long start,end; final String text;
        Word(DataInputStream in)throws IOException {start=in.readLong();end=in.readLong();text=string(in);if(start<0||end<=start)throw new IOException("word time");}
    }
    static final class Line {
        final long start,end; final String text,translation,phonetic; final List<Word> words;
        Line(DataInputStream in)throws IOException {
            start=in.readLong();end=in.readLong();text=string(in);translation=string(in);phonetic=string(in);
            if(start<0||end<=start)throw new IOException("line time");
            int count=count(in,4096);List<Word> w=new ArrayList<>(count);
            for(int i=0;i<count;i++)w.add(new Word(in));words=Collections.unmodifiableList(w);
        }
    }
    final long track,revision,duration;
    final String title,artist,album;
    final List<Line> lines;
    Document(byte[] data)throws IOException {
        if(data==null||data.length>4*1024*1024)throw new IOException("document size");
        DataInputStream in=new DataInputStream(new ByteArrayInputStream(data));
        if(in.readInt()!=0x4c595231)throw new IOException("document version");
        track=in.readLong();revision=in.readLong();duration=in.readLong();
        if(duration<0)throw new IOException("duration");
        title=string(in);artist=string(in);album=string(in);
        int count=count(in,32768);List<Line> l=new ArrayList<>(count);
        for(int i=0;i<count;i++)l.add(new Line(in));
        if(in.available()!=0)throw new IOException("trailing bytes");
        lines=Collections.unmodifiableList(l);
    }
    private static int count(DataInputStream in,int max)throws IOException {int n=in.readInt();if(n<0||n>max)throw new IOException("count");return n;}
    private static String string(DataInputStream in)throws IOException {
        int n=count(in,65536);if(n>in.available())throw new IOException("truncated string");
        byte[] b=new byte[n];in.readFully(b);return new String(b,StandardCharsets.UTF_8);
    }
}
