#include "platform.h"
#include "json.hpp"
#include <jni.h>
#include <unistd.h>
#include <chrono>
namespace sources {
namespace {
using Json=nlohmann::json;
JavaVM *vm=nullptr;
jobject activity=nullptr;
struct Env {
    JNIEnv *env=nullptr;
    bool attached=false;
    Env(){
        if(!vm)return;
        if(vm->GetEnv(reinterpret_cast<void **>(&env),JNI_VERSION_1_6)==JNI_EDETACHED)
            attached=vm->AttachCurrentThread(&env,nullptr)==0;
        if(env && env->PushLocalFrame(48)<0){env->ExceptionClear();if(attached)vm->DetachCurrentThread();env=nullptr;}
    }
    ~Env(){if(env){env->ExceptionClear();env->PopLocalFrame(nullptr);if(attached)vm->DetachCurrentThread();}}
    bool failed() const {if(!env)return true;if(!env->ExceptionCheck())return false;env->ExceptionClear();return true;}
    jmethodID method(jobject obj,const char *name,const char *signature) const {
        if(!obj)return nullptr;
        auto type=env->GetObjectClass(obj);
        if(failed() || !type)return nullptr;
        auto id=env->GetMethodID(type,name,signature);
        env->DeleteLocalRef(type);
        return failed()?nullptr:id;
    }
};
std::string string(Env &e,jstring value){
    if(e.failed() || !value)return {};
    const char *utf=e.env->GetStringUTFChars(value,nullptr);
    if(!utf)return {};
    std::string result=utf;e.env->ReleaseStringUTFChars(value,utf);return result;
}
}
bool initialize(const LyraHostV3 *host){
    vm=static_cast<JavaVM *>(host->java_vm);
    Env e;
    if(!e.env || !host->android_context)return false;
    auto object=static_cast<jobject>(host->android_context);
    auto version=e.method(object,"lyraPlatformVersion","()I");
    if(!version || e.env->CallIntMethod(object,version)!=9 || e.failed())return false;
    activity=e.env->NewGlobalRef(object);
    return activity!=nullptr;
}
void disconnect(){
    Env e;if(e.env && activity)e.env->DeleteGlobalRef(activity);activity=nullptr;
}
std::string files_directory(){
    Env e;if(!e.env)return {};
    auto get=e.method(activity,"getFilesDir","()Ljava/io/File;");
    if(!get)return {};
    auto file=e.env->CallObjectMethod(activity,get);
    if(e.failed())return {};
    auto path=e.method(file,"getAbsolutePath","()Ljava/lang/String;");
    return path?string(e,static_cast<jstring>(e.env->CallObjectMethod(file,path))):std::string{};
}
bool choose_document(){
    Env e;if(!e.env)return false;
    auto choose=e.method(activity,"chooseDocument","(Ljava/lang/String;Ljava/lang/String;)V");
    if(!choose)return false;
    e.env->CallVoidMethod(activity,choose,e.env->NewStringUTF("选择音源脚本"),e.env->NewStringUTF(""));
    return !e.failed();
}
int take_document(){
    Env e;if(!e.env)return -3;
    auto take=e.method(activity,"takeDocumentFd","()I");
    if(!take)return -3;
    auto fd=e.env->CallIntMethod(activity,take);
    return e.failed()?-3:fd;
}
std::string read_file(int fd,size_t limit){
    if(fd<0)return {};
    std::string text;char buffer[8192];
    for(;;){auto n=read(fd,buffer,sizeof(buffer));if(n==0)break;
        if(n<0 || size_t(n)>limit-text.size()){text.clear();break;}text.append(buffer,size_t(n));}
    close(fd);return text;
}
static std::string fetch(std::string_view url,std::string_view verb,std::string_view headers,std::string_view body,bool binary,int redirects=3){
    if(binary && url.size()>8192)return {};
    const char *failure=binary?"":R"({"status":0,"body":""})";
    if(url.substr(0,8)!="https://" || url.size()>8192 || url.find('\0')!=url.npos ||
       (verb!="GET" && verb!="POST" && verb!="HEAD") || body.size()>1024*1024)return failure;
    Env e;if(!e.env)return failure;
    auto *j=e.env;
    const auto url_type=j->FindClass("java/net/URL");
    if(e.failed() || !url_type)return failure;
    auto ctor=j->GetMethodID(url_type,"<init>","(Ljava/lang/String;)V");
    auto open=j->GetMethodID(url_type,"openConnection","()Ljava/net/URLConnection;");
    if(e.failed() || !ctor || !open)return failure;
    auto address=j->NewObject(url_type,ctor,j->NewStringUTF(std::string(url).c_str()));
    if(e.failed() || !address)return failure;
    auto connection=j->CallObjectMethod(address,open);
    if(e.failed() || !connection)return failure;
    auto disconnect=e.method(connection,"disconnect","()V");
    auto run=[&]()->std::string {
        auto connect=e.method(connection,"setConnectTimeout","(I)V");
        auto timeout=e.method(connection,"setReadTimeout","(I)V");
        auto redirect=e.method(connection,"setInstanceFollowRedirects","(Z)V");
        auto method=e.method(connection,"setRequestMethod","(Ljava/lang/String;)V");
        auto header=e.method(connection,"setRequestProperty","(Ljava/lang/String;Ljava/lang/String;)V");
        auto status=e.method(connection,"getResponseCode","()I");
        if(!connect || !timeout || !redirect || !method || !header || !status)return failure;
        j->CallVoidMethod(connection,connect,1800);
        j->CallVoidMethod(connection,timeout,1800);
        j->CallVoidMethod(connection,redirect,JNI_FALSE);
        j->CallVoidMethod(connection,method,j->NewStringUTF(std::string(verb).c_str()));
        if(e.failed())return failure;
        auto fields=Json::parse(headers.begin(),headers.end(),nullptr,false);
        if(!fields.is_object() || fields.size()>24)return failure;
        for(const auto &[key,value]:fields.items()){
            if(!value.is_string() || key.find_first_of("\r\n")!=key.npos)continue;
            auto &text=value.get_ref<const std::string &>();
            if(text.size()>8192 || text.find_first_of("\r\n")!=text.npos)continue;
            auto name=j->NewStringUTF(key.c_str()),val=j->NewStringUTF(text.c_str());
            if(e.failed())return failure;
            j->CallVoidMethod(connection,header,name,val);
            j->DeleteLocalRef(name);j->DeleteLocalRef(val);
            if(e.failed())return failure;
        }
        if(verb=="POST"){
            auto output=e.method(connection,"setDoOutput","(Z)V");
            auto stream=e.method(connection,"getOutputStream","()Ljava/io/OutputStream;");
            if(!output || !stream)return failure;
            j->CallVoidMethod(connection,output,JNI_TRUE);
            auto out=j->CallObjectMethod(connection,stream);
            if(e.failed() || !out)return failure;
            auto write=e.method(out,"write","([B)V");
            auto close=e.method(out,"close","()V");
            auto buffer=j->NewByteArray(jsize(body.size()));
            if(e.failed() || !buffer || !write || !close)return failure;
            j->SetByteArrayRegion(buffer,0,jsize(body.size()),reinterpret_cast<const jbyte *>(body.data()));
            j->CallVoidMethod(out,write,buffer);
            bool error=e.failed();
            j->CallVoidMethod(out,close);
            if(e.failed() || error)return failure;
        }
        auto code=j->CallIntMethod(connection,status);
        if(e.failed())return failure;
        if(redirects>0 && (code==301 || code==302 || code==303 || code==307 || code==308) && verb!="POST"){
            auto field=e.method(connection,"getHeaderField","(Ljava/lang/String;)Ljava/lang/String;");
            auto relative=j->GetMethodID(url_type,"<init>","(Ljava/net/URL;Ljava/lang/String;)V");
            auto external=j->GetMethodID(url_type,"toExternalForm","()Ljava/lang/String;");
            if(e.failed() || !field || !relative || !external)return failure;
            auto location=j->CallObjectMethod(connection,field,j->NewStringUTF("Location"));
            if(e.failed() || !location)return failure;
            auto target=j->NewObject(url_type,relative,address,location);
            if(e.failed() || !target)return failure;
            auto next=string(e,static_cast<jstring>(j->CallObjectMethod(target,external)));
            return fetch(next,verb,"{}","",binary,redirects-1);
        }
        std::string text;
        if(verb!="HEAD" && code>=200 && code<300){
            auto stream=e.method(connection,"getInputStream","()Ljava/io/InputStream;");
            if(!stream)return failure;
            auto in=j->CallObjectMethod(connection,stream);
            if(e.failed() || !in)return failure;
            auto read=e.method(in,"read","([B)I");
            auto close=e.method(in,"close","()V");
            constexpr jsize chunk=8192;
            auto buffer=j->NewByteArray(chunk);
            if(!read || !close || e.failed() || !buffer)return failure;
            bool ok=true;
            auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(3500);
            for(;;){
                if(std::chrono::steady_clock::now()>=deadline){ok=false;break;}
                auto n=j->CallIntMethod(in,read,buffer);
                if(e.failed()){ok=false;break;}
                if(n<0)break;
                if(!n || text.size()+size_t(n)>2*1024*1024){ok=false;break;}
                auto offset=text.size();text.resize(offset+size_t(n));
                j->GetByteArrayRegion(buffer,0,n,reinterpret_cast<jbyte *>(text.data()+offset));
                if(e.failed()){ok=false;break;}
            }
            j->CallVoidMethod(in,close);
            if(e.failed() || !ok)return failure;
        }
        if(binary)return code>=200 && code<300?text:std::string{};
        Json response_headers=Json::object();
        if(auto get_header=e.method(connection,"getHeaderField","(Ljava/lang/String;)Ljava/lang/String;"))
            for(const char *name:{"content-type","location","retry-after","etag"}){
                auto key=j->NewStringUTF(name);
                auto value=static_cast<jstring>(j->CallObjectMethod(connection,get_header,key));
                auto text_value=string(e,value);
                if(!text_value.empty())response_headers[name]=std::move(text_value);
                j->DeleteLocalRef(key);if(value)j->DeleteLocalRef(value);
            }
        return Json{{"status",code},{"body",std::move(text)},{"headers",std::move(response_headers)}}.dump(-1,' ',false,Json::error_handler_t::replace);
    };
    auto result=run();
    if(disconnect)j->CallVoidMethod(connection,disconnect);
    return result;
}
std::string network(std::string_view url,std::string_view method,std::string_view headers,std::string_view body){
    return fetch(url,method,headers,body,false);
}
std::string decode_charset(std::string_view bytes,const char *encoding){
    Env e;if(!e.env)return {};
    auto *j=e.env;
    auto cls=j->FindClass("java/lang/String");
    if(e.failed() || !cls)return {};
    auto ctor=j->GetMethodID(cls,"<init>","([BLjava/lang/String;)V");
    auto out=j->GetMethodID(cls,"getBytes","(Ljava/lang/String;)[B");
    auto input=j->NewByteArray(jsize(bytes.size()));
    if(e.failed() || !ctor || !out || !input)return {};
    j->SetByteArrayRegion(input,0,jsize(bytes.size()),reinterpret_cast<const jbyte *>(bytes.data()));
    auto text=j->NewObject(cls,ctor,input,j->NewStringUTF(encoding));
    if(e.failed() || !text)return {};
    auto encoded=static_cast<jbyteArray>(j->CallObjectMethod(text,out,j->NewStringUTF("UTF-8")));
    if(e.failed() || !encoded)return {};
    std::string result(size_t(j->GetArrayLength(encoded)),'\0');
    j->GetByteArrayRegion(encoded,0,jsize(result.size()),reinterpret_cast<jbyte *>(result.data()));
    return e.failed()?std::string{}:result;
}
std::string icon_tile(std::string_view url){
    constexpr jsize side=32;
    Env e;if(!e.env)return {};
    auto *j=e.env;
    jbyteArray bytes=nullptr;
    if(url.substr(0,5)=="data:" && url.size()<=256*1024){
        auto split=url.find(";base64,");
        if(split==url.npos)return {};
        auto base64=j->FindClass("android/util/Base64");
        if(e.failed() || !base64)return {};
        auto decode=j->GetStaticMethodID(base64,"decode","(Ljava/lang/String;I)[B");
        if(e.failed() || !decode)return {};
        bytes=static_cast<jbyteArray>(j->CallStaticObjectMethod(base64,decode,
            j->NewStringUTF(std::string(url.substr(split+8)).c_str()),0));
    }else{
        auto image=fetch(url,"GET","{}","",true);
        if(image.empty() || image.size()>256*1024)return {};
        bytes=j->NewByteArray(jsize(image.size()));
        if(e.failed() || !bytes)return {};
        j->SetByteArrayRegion(bytes,0,jsize(image.size()),reinterpret_cast<const jbyte *>(image.data()));
    }
    if(e.failed() || !bytes)return {};
    auto factory=j->FindClass("android/graphics/BitmapFactory");
    auto options=j->FindClass("android/graphics/BitmapFactory$Options");
    if(e.failed() || !factory || !options)return {};
    auto decode=j->GetStaticMethodID(factory,"decodeByteArray","([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;");
    auto ctor=j->GetMethodID(options,"<init>","()V");
    auto bounds=j->GetFieldID(options,"inJustDecodeBounds","Z");
    auto w=j->GetFieldID(options,"outWidth","I"),h=j->GetFieldID(options,"outHeight","I");
    if(e.failed() || !decode || !ctor || !bounds || !w || !h)return {};
    auto opt=j->NewObject(options,ctor);
    if(e.failed() || !opt)return {};
    j->SetBooleanField(opt,bounds,JNI_TRUE);
    auto length=j->GetArrayLength(bytes);
    j->CallStaticObjectMethod(factory,decode,bytes,0,length,opt);
    if(e.failed())return {};
    auto width=j->GetIntField(opt,w),height=j->GetIntField(opt,h);
    if(width<=0 || height<=0 || width>2048 || height>2048)return {};
    j->SetBooleanField(opt,bounds,JNI_FALSE);
    auto bitmap=j->CallStaticObjectMethod(factory,decode,bytes,0,length,opt);
    if(e.failed() || !bitmap)return {};
    auto type=j->GetObjectClass(bitmap);
    auto scale=j->GetStaticMethodID(type,"createScaledBitmap","(Landroid/graphics/Bitmap;IIZ)Landroid/graphics/Bitmap;");
    if(e.failed() || !scale)return {};
    auto tile=j->CallStaticObjectMethod(type,scale,bitmap,side,side,JNI_TRUE);
    if(e.failed() || !tile)return {};
    auto pixels=e.method(tile,"getPixels","([IIIIIII)V");
    auto argb=j->NewIntArray(side*side);
    if(!pixels || e.failed() || !argb)return {};
    j->CallVoidMethod(tile,pixels,argb,0,side,0,0,side,side);
    if(e.failed())return {};
    jint values[side*side];
    j->GetIntArrayRegion(argb,0,side*side,values);
    if(e.failed())return {};
    std::string hex;
    hex.reserve(side*side*8);
    constexpr char digits[]="0123456789abcdef";
    for(uint32_t value:values){
        unsigned alpha=value>>24;
        for(unsigned shift:{16u,8u,0u,24u}){
            unsigned byte=(value>>shift)&255;
            if(shift!=24)byte=(byte*alpha+127)/255;
            hex+=digits[byte>>4];hex+=digits[byte&15];
        }
    }
    return hex;
}

}
