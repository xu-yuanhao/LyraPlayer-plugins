#pragma once
#include <cstdarg>
enum LyraLogPriority { LyraLogDebug=3,LyraLogInfo=4,LyraLogWarn=5,LyraLogError=6 };
int lyra_log(int priority,const char* tag,const char* format,...);
int lyra_vlog(int priority,const char* tag,const char* format,va_list arguments);
