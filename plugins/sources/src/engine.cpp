#include "engine.h"
#include "quickjs.h"
#include "adapter.inc"
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include "platform.h"
#endif

namespace sources {
namespace {
using Clock = std::chrono::steady_clock;
struct Run {
    Transport transport;
    Clock::time_point deadline;
};
int interrupt(JSRuntime *,void *opaque) {
    return Clock::now() >= static_cast<const Run *>(opaque)->deadline;
}
JSValue settle(JSContext *context,JSRuntime *runtime,JSValue promise,const Run &run) {
    if (JS_IsException(promise)) return promise;
    auto global=JS_GetGlobalObject(context);
    auto timer=JS_GetPropertyStr(context,global,"__runTimer");
    JS_FreeValue(context,global);
    JSContext *job=nullptr;
    while (JS_PromiseState(context,promise)==JS_PROMISE_PENDING && Clock::now()<run.deadline){
        const int pending=JS_ExecutePendingJob(runtime,&job);
        if(pending<0)break;
        if(pending>0)continue;
        auto tick=JS_Call(context,timer,JS_UNDEFINED,0,nullptr);
        const bool ran=JS_IsBool(tick) && JS_ToBool(context,tick);
        JS_FreeValue(context,tick);
        if(!ran)break;
    }
    JS_FreeValue(context,timer);
    auto value=JS_PromiseState(context,promise)==JS_PROMISE_FULFILLED
        ? JS_PromiseResult(context,promise) : JS_EXCEPTION;
    JS_FreeValue(context,promise);
    return value;
}
JSValue wait_timer(JSContext *context,JSValueConst,int count,JSValueConst *args){
    auto &run=*static_cast<const Run *>(JS_GetContextOpaque(context));
    double delay=0;
    if(count!=1 || JS_ToFloat64(context,&delay,args[0]))return JS_EXCEPTION;
    const auto remaining=std::chrono::duration<double,std::milli>(run.deadline-Clock::now());
    if(!(delay<remaining.count()) || remaining.count()<=0)return JS_FALSE;
    if(delay>0)std::this_thread::sleep_for(std::chrono::duration<double,std::milli>(delay));
    return JS_NewBool(context,Clock::now()<run.deadline);
}
JSValue random_bytes(JSContext *context,JSValueConst,int count,JSValueConst *args) {
    uint32_t size=0;
    if(count!=1 || JS_ToUint32(context,&size,args[0]) || size>65536)
        return JS_ThrowTypeError(context,"Invalid random byte count");
    std::vector<uint8_t> data(size);
    int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(fd<0)return JS_ThrowInternalError(context,"System randomness unavailable");
    size_t at=0;
    while(at<data.size()){
        auto n=read(fd,data.data()+at,data.size()-at);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        at+=size_t(n);
    }
    close(fd);
    if(at!=data.size())return JS_ThrowInternalError(context,"System randomness unavailable");
    return JS_NewArrayBufferCopy(context,data.data(),data.size());
}
JSValue decode_text(JSContext *context,JSValueConst,int count,JSValueConst *args){
    if(count!=2)return JS_ThrowTypeError(context,"Invalid text input");
    size_t offset=0,length=0,width=0,size=0;
    auto buffer=JS_GetTypedArrayBuffer(context,args[0],&offset,&length,&width);
    if(JS_IsException(buffer))return buffer;
    auto *start=JS_GetArrayBuffer(context,&size,buffer);
    auto *encoding=JS_ToCString(context,args[1]);
    JSValue value=JS_UNDEFINED;
    if(!encoding || length>2*1024*1024 || offset>size || length>size-offset || (!start && length))
        value=JS_ThrowTypeError(context,"Invalid text buffer");
    else {
        std::string_view bytes(length?reinterpret_cast<const char *>(start+offset):"",length);
#ifdef __ANDROID__
        auto decoded=decode_charset(bytes,encoding);
        value=JS_NewStringLen(context,decoded.data(),decoded.size());
#else
        if(std::string_view(encoding)=="utf-8" || std::string_view(encoding)=="utf8")
            value=JS_NewStringLen(context,bytes.data(),bytes.size());
        else value=JS_ThrowTypeError(context,"Unsupported fixture encoding");
#endif
    }
    if(encoding)JS_FreeCString(context,encoding);
    JS_FreeValue(context,buffer);
    return value;
}
JSValue fetch(JSContext *context,JSValueConst,int count,JSValueConst *args) {
    auto &run = *static_cast<Run *>(JS_GetContextOpaque(context));
    if (count != 4 || !run.transport || Clock::now() >= run.deadline)
        return JS_ThrowTypeError(context,"Network unavailable");
    std::string text[4];
    for (int i=0;i<count;++i) {
        size_t size=0;
        auto *value=JS_ToCStringLen(context,&size,args[i]);
        if (!value) return JS_EXCEPTION;
        text[i].assign(value,size);
        JS_FreeCString(context,value);
    }
    auto response=run.transport(text[0],text[1],text[2],text[3]);
    return JS_NewStringLen(context,response.data(),response.size());
}
}
std::string evaluate(std::string_view script,std::string_view operation,
                     std::string_view arguments,Transport transport,int timeout_seconds) {
    if (script.empty() || script.size()>1024*1024) return {};
    Run run{std::move(transport),Clock::now()+std::chrono::seconds(timeout_seconds)};
    auto *runtime=JS_NewRuntime();
    if (!runtime) return {};
    JS_SetMemoryLimit(runtime,24*1024*1024);
    JS_SetMaxStackSize(runtime,512*1024);
    JS_SetInterruptHandler(runtime,interrupt,&run);
    auto *context=JS_NewContext(runtime);
    std::string output;
    if (context) {
        JS_SetContextOpaque(context,&run);
        auto global=JS_GetGlobalObject(context);
        JS_SetPropertyStr(context,global,"__waitTimer",JS_NewCFunction(context,wait_timer,"__waitTimer",1));
        JS_SetPropertyStr(context,global,"__transport",JS_NewCFunction(context,fetch,"__transport",4));
        JS_SetPropertyStr(context,global,"__randomBytes",JS_NewCFunction(context,random_bytes,"__randomBytes",1));
        JS_SetPropertyStr(context,global,"__decodeText",JS_NewCFunction(context,decode_text,"__decodeText",2));
        auto initialized=JS_Eval(context,reinterpret_cast<const char *>(adapter),sizeof(adapter)-1,"adapter.js",JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(initialized)) {
            auto load=JS_GetPropertyStr(context,global,"__load");
            auto code=JS_NewStringLen(context,script.data(),script.size());
            auto plugin=settle(context,runtime,JS_Call(context,load,JS_UNDEFINED,1,&code),run);
            if (!JS_IsException(plugin)) {
                const bool metadata=operation=="metadata";
                auto function=JS_GetPropertyStr(context,global,metadata?"__metadata":"__invoke");
                JSValue args[]={plugin,JS_NewStringLen(context,operation.data(),operation.size()),
                    JS_ParseJSON(context,arguments.data(),arguments.size(),"args")};
                auto result=JS_Call(context,function,JS_UNDEFINED,metadata?1:3,args);
                if (!metadata) result=settle(context,runtime,result,run);
                auto json=JS_JSONStringify(context,result,JS_UNDEFINED,JS_UNDEFINED);
                if (JS_IsString(json)) {
                    size_t size=0;
                    if (auto *value=JS_ToCStringLen(context,&size,json)) {
                        if (size<=1024*1024) output.assign(value,size);
                        JS_FreeCString(context,value);
                    }
                }
                JS_FreeValue(context,json);
                JS_FreeValue(context,result);
                JS_FreeValue(context,args[1]);
                JS_FreeValue(context,args[2]);
                JS_FreeValue(context,function);
            }
            JS_FreeValue(context,plugin);
            JS_FreeValue(context,code);
            JS_FreeValue(context,load);
        }
        JS_FreeValue(context,initialized);
        JS_FreeValue(context,global);
        JS_FreeContext(context);
    }
    JS_FreeRuntime(runtime);
    return output;
}
}
