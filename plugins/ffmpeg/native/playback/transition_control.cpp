#include "playback/transition_control.h"
#include <cmath>
#include <array>
#include <cstring>
namespace {
const LyraFieldRuleV3 rules[]={
 {LYRA_TRANSITION_ENABLED,LYRA_FIELD_I64},
 {LYRA_TRANSITION_AUTOMATIC_MS,LYRA_FIELD_F64},
 {LYRA_TRANSITION_MANUAL_MS,LYRA_FIELD_F64}};
bool valid(const LyraAudioTransitionV3 *api){return api && api->size==sizeof(*api) && api->version==LYRA_INTERFACE_VERSION && api->request;}
}
AudioTransitionControl::AudioTransitionControl(){settings_={true,7000.,4000.};api_={sizeof(api_),LYRA_INTERFACE_VERSION,this,request};}
int AudioTransitionControl::request(void *user,const LyraMessageV3 *message,LyraReplyV3 *reply){
 if(!reply || reply->size!=sizeof(*reply))return LYRA_MESSAGE_INVALID;
 reply->field_count=0;reply->status=LYRA_MESSAGE_INVALID;
 if(!message || message->size!=sizeof(*message))return reply->status;
 auto &self=*static_cast<AudioTransitionControl*>(user);
 if(message->operation==LYRA_TRANSITION_QUERY){
  if(lyra_message_fields(message,nullptr,0)<0)return reply->status;
  if(reply->capacity<2 || !reply->fields)return reply->status=LYRA_MESSAGE_CAPACITY;
  std::lock_guard<std::mutex> lock(self.mutex_);
  self.reply_enabled_=self.settings_.enabled;self.reply_ms_=self.settings_.automatic_ms;
  reply->fields[0]={LYRA_TRANSITION_ENABLED,LYRA_FIELD_I64,0,sizeof(self.reply_enabled_),&self.reply_enabled_};
  reply->fields[1]={LYRA_TRANSITION_AUTOMATIC_MS,LYRA_FIELD_F64,0,sizeof(self.reply_ms_),&self.reply_ms_};
  reply->field_count=2;return reply->status=LYRA_MESSAGE_OK;
 }
 if(message->operation!=LYRA_TRANSITION_CONFIGURE)return reply->status=LYRA_MESSAGE_UNSUPPORTED;
 if(lyra_message_fields(message,rules,sizeof(rules)/sizeof(rules[0]))<0)return reply->status;
 AudioTransitionSettings s;
 bool seen[sizeof(rules)/sizeof(rules[0])]{};
 for(uint32_t i=0;i<message->field_count;++i){const auto &f=message->fields[i];
  if(f.tag<LYRA_TRANSITION_ENABLED || f.tag>LYRA_TRANSITION_MANUAL_MS)continue;
  auto n=f.tag-LYRA_TRANSITION_ENABLED;if(seen[n] || !f.data)return reply->status;seen[n]=true;
  if(n==0){int64_t value=0;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));if(value!=0 && value!=1)return reply->status;s.enabled=value!=0;}
  else{double value=0;if(f.bytes!=sizeof(value))return reply->status;memcpy(&value,f.data,sizeof(value));if(!std::isfinite(value)||value<=0)return reply->status;f.tag==LYRA_TRANSITION_AUTOMATIC_MS?s.automatic_ms=value:s.manual_ms=value;}
 }
 for(bool present:seen)if(!present)return reply->status;
 if(!audio_transition_valid(s))return reply->status;
 {std::lock_guard<std::mutex> lock(self.mutex_);self.settings_=s;}
 return reply->status=LYRA_MESSAGE_OK;
}
int configure_transition(const LyraAudioTransitionV3 *api,const AudioTransitionSettings &s){
 if(!valid(api))return LYRA_MESSAGE_UNSUPPORTED;
 if(!audio_transition_valid(s))return LYRA_MESSAGE_INVALID;
 int64_t enabled=s.enabled;
 double values[]={s.automatic_ms,s.manual_ms};
 LyraFieldV3 fields[sizeof(rules)/sizeof(rules[0])]{};
 fields[0]={LYRA_TRANSITION_ENABLED,LYRA_FIELD_I64,LYRA_FIELD_REQUIRED,sizeof(enabled),&enabled};
 for(size_t i=1;i<std::size(fields);++i)fields[i]={rules[i].tag,LYRA_FIELD_F64,LYRA_FIELD_REQUIRED,sizeof(double),&values[i-1]};
 LyraMessageV3 message{sizeof(message),uint32_t(std::size(fields)),LYRA_TRANSITION_CONFIGURE,fields};LyraReplyV3 reply{sizeof(reply),0,0,0,nullptr};
 return api->request(api->user,&message,&reply);
}
double automatic_transition_ms(const LyraAudioTransitionV3 *api){
 if(!valid(api))return 0;
 LyraMessageV3 message{sizeof(message),0,LYRA_TRANSITION_QUERY,nullptr};LyraFieldV3 fields[2]{};LyraReplyV3 reply{sizeof(reply),2,0,0,fields};
 if(api->request(api->user,&message,&reply)!=0 || reply.status!=LYRA_MESSAGE_OK || reply.size!=sizeof(reply) ||
    reply.capacity!=std::size(fields) || reply.fields!=fields || reply.field_count>std::size(fields))return 0;
 int64_t enabled=0;double ms=0;
 for(uint32_t i=0;i<reply.field_count;++i){const auto &f=fields[i];if(!f.data)continue;
  if(f.tag==LYRA_TRANSITION_ENABLED && f.type==LYRA_FIELD_I64 && f.bytes==sizeof(enabled))memcpy(&enabled,f.data,sizeof(enabled));
  if(f.tag==LYRA_TRANSITION_AUTOMATIC_MS && f.type==LYRA_FIELD_F64 && f.bytes==sizeof(ms))memcpy(&ms,f.data,sizeof(ms));
 }
 return enabled==1 && std::isfinite(ms) && ms>0 && ms<=8000?ms:0;
}
