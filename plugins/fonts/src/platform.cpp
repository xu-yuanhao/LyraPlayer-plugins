#include "platform.h"
#include <jni.h>
namespace {
struct Call {
 JavaVM *vm=nullptr;JNIEnv *env=nullptr;jobject object=nullptr;jclass cls=nullptr;bool attached=false;
 explicit Call(const LyraHostV3 *h){if(!h || !h->java_vm || !h->android_context)return;vm=static_cast<JavaVM*>(h->java_vm);object=static_cast<jobject>(h->android_context);
  if(vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK){if(vm->AttachCurrentThread(&env,nullptr)!=JNI_OK){env=nullptr;return;}attached=true;}
  cls=env->GetObjectClass(object);
 }
 ~Call(){if(env){if(env->ExceptionCheck())env->ExceptionClear();if(cls)env->DeleteLocalRef(cls);}if(attached)vm->DetachCurrentThread();}
 jmethodID method(const char *name,const char *signature){return env && cls?env->GetMethodID(cls,name,signature):nullptr;}
 bool ok(){if(!env || env->ExceptionCheck()){if(env)env->ExceptionClear();return false;}return true;}
};
}
bool font_platform_ready(const LyraHostV3*h){Call c(h);auto id=c.method("lyraPlatformVersion","()I");return id && c.env->CallIntMethod(c.object,id)==8 && c.ok();}
std::string font_storage(const LyraHostV3*h){Call c(h);auto id=c.method("getFilesDir","()Ljava/io/File;");if(!id)return {};auto file=c.env->CallObjectMethod(c.object,id);if(!file || !c.ok())return {};auto cls=c.env->GetObjectClass(file);auto path=c.env->GetMethodID(cls,"getAbsolutePath","()Ljava/lang/String;");auto str=path?static_cast<jstring>(c.env->CallObjectMethod(file,path)):nullptr;std::string out;
 if(str && c.ok()){auto *p=c.env->GetStringUTFChars(str,nullptr);if(p){out=p;c.env->ReleaseStringUTFChars(str,p);}c.env->DeleteLocalRef(str);}c.env->DeleteLocalRef(cls);c.env->DeleteLocalRef(file);return out;
}
int font_pick(const LyraHostV3*h,const char *title,const char *mime){Call c(h);auto id=c.method("chooseDocument","(Ljava/lang/String;Ljava/lang/String;)V");if(!id)return -1;auto a=c.env->NewStringUTF(title),b=c.env->NewStringUTF(mime);if(a && b)c.env->CallVoidMethod(c.object,id,a,b);bool okay=a && b && c.ok();if(a)c.env->DeleteLocalRef(a);if(b)c.env->DeleteLocalRef(b);return okay?0:-1;}
int font_pick_result(const LyraHostV3*h){Call c(h);auto id=c.method("takeDocumentFd","()I");if(!id)return -2;int fd=c.env->CallIntMethod(c.object,id);return c.ok()?fd:-2;}
