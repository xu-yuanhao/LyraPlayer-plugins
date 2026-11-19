#include "engine.h"
#include "quickjs.h"
#include "adapter.inc"
#include <chrono>

namespace sources {
namespace {
using Clock = std::chrono::steady_clock;
struct Run {
    Transport transport;
    Clock::time_point deadline = Clock::now() + std::chrono::seconds(5);
};
int interrupt(JSRuntime *,void *opaque) {
    return Clock::now() >= static_cast<const Run *>(opaque)->deadline;
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
                     std::string_view arguments,Transport transport) {
    if (script.empty() || script.size()>1024*1024) return {};
    Run run{std::move(transport)};
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
        JS_SetPropertyStr(context,global,"__transport",JS_NewCFunction(context,fetch,"__transport",4));
        auto initialized=JS_Eval(context,reinterpret_cast<const char *>(adapter),sizeof(adapter)-1,"adapter.js",JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(initialized)) {
            auto load=JS_GetPropertyStr(context,global,"__load");
            auto code=JS_NewStringLen(context,script.data(),script.size());
            auto plugin=JS_Call(context,load,JS_UNDEFINED,1,&code);
            if (!JS_IsException(plugin)) {
                const bool metadata=operation=="metadata";
                auto function=JS_GetPropertyStr(context,global,metadata?"__metadata":"__invoke");
                JSValue args[]={plugin,JS_NewStringLen(context,operation.data(),operation.size()),
                    JS_ParseJSON(context,arguments.data(),arguments.size(),"args")};
                auto result=JS_Call(context,function,JS_UNDEFINED,metadata?1:3,args);
                if (!metadata && !JS_IsException(result)) {
                    JSContext *job=nullptr;
                    while (JS_PromiseState(context,result)==JS_PROMISE_PENDING && Clock::now()<run.deadline)
                        if (JS_ExecutePendingJob(runtime,&job)<=0) break;
                    auto next=JS_PromiseState(context,result)==JS_PROMISE_FULFILLED
                        ? JS_PromiseResult(context,result) : JS_UNDEFINED;
                    JS_FreeValue(context,result);
                    result=next;
                }
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
