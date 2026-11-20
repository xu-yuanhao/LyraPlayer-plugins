#pragma once
#include "contracts.h"
#include "message_contracts.h"
#define LYRA_INTERFACE_SERVICE_CALL 28u
// Synchronous frame-thread typed endpoint dispatch. HANDLE is a live registry
// capability; INTERFACE is an allocated message-interface ID. MESSAGE is BYTES
// of exactly sizeof(LyraMessageV3), copied before validation. Its field table and
// data are borrowed for this call only: this is not a wire/file representation.
// Dispatch establishes target ownership across query and request, prevents
// unloading an active implementation, and validates the reply. Frozen non-message
// interfaces are never reinterpreted as message endpoints.
enum {LYRA_SERVICE_CALL=1};
enum {LYRA_SERVICE_CALL_HANDLE=1,LYRA_SERVICE_CALL_INTERFACE=2,LYRA_SERVICE_CALL_MESSAGE=3};
typedef struct LyraServiceCallV3 {
 uint32_t size,version;
 void *user;
 int (*request)(void *,const LyraMessageV3 *,LyraReplyV3 *);
} LyraServiceCallV3;
// C++ callers share one envelope builder instead of expanding protocol glue.
#ifdef __cplusplus
#if defined(__GNUC__)
__attribute__((noinline))
#endif
inline
#else
static inline
#endif
int lyra_service_call(const LyraServiceCallV3 *api,uint64_t handle,
                                   uint32_t interface_id,const LyraMessageV3 *message,LyraReplyV3 *reply) {
 if(!api || api->size!=sizeof(*api) || api->version!=LYRA_INTERFACE_VERSION || !api->request || !message || !reply)return LYRA_MESSAGE_INVALID;
 int64_t id=interface_id;
 LyraFieldV3 fields[]={{LYRA_SERVICE_CALL_HANDLE,LYRA_FIELD_HANDLE,LYRA_FIELD_REQUIRED,sizeof(handle),&handle},
                     {LYRA_SERVICE_CALL_INTERFACE,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(id),&id},
                     {LYRA_SERVICE_CALL_MESSAGE,LYRA_FIELD_BYTES,LYRA_FIELD_REQUIRED,sizeof(*message),message}};
 LyraMessageV3 call={sizeof(call),sizeof(fields)/sizeof(*fields),LYRA_SERVICE_CALL,fields};
 return api->request(api->user,&call,reply);
}
