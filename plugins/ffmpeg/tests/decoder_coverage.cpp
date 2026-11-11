// Compare upstream audio registry size against the linked runtime's media types.
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <cstdio>
#include <fstream>
#include <string>
#include <algorithm>
int main(int argc,char **argv){
    if(argc!=2)return 2;
    std::ifstream file(argv[1]);std::string list;std::getline(file,list);
    if(list.empty())return 2;
    size_t expected=1+std::count(list.begin(),list.end(),','),count=0;
    void *state=nullptr;const AVCodec *codec;
    while((codec=av_codec_iterate(&state))){
        if(!av_codec_is_decoder(codec) || codec->type!=AVMEDIA_TYPE_AUDIO){
            std::printf("Unexpected codec: %s type=%d\n",codec->name,int(codec->type));return 3;
        }
        ++count;
    }
    std::printf("Linked audio decoders: %zu; upstream audio registry: %zu\n",count,expected);
    return count==expected?0:1;
}
