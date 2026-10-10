#include "platform/log.h"
#include <android/log.h>
int lyra_vlog(int priority,const char* tag,const char* format,va_list arguments){return __android_log_vprint(priority,tag,format,arguments);}
int lyra_log(int priority,const char* tag,const char* format,...){va_list args;va_start(args,format);int n=lyra_vlog(priority,tag,format,args);va_end(args);return n;}
