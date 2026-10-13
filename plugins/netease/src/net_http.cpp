// HTTPS via JNI HttpURLConnection with manual redirect handling.
// Runs on plugin worker threads; attaches to the JVM per call. Only the
// cached JavaVM is used, never a cached Activity: the Activity may be
// recreated while the plugin lives, but the VM is process-global.
#include "net_http.h"

#include <jni.h>

namespace netease {
namespace {

const char *kDesktopUa =
    "Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like "
    "Gecko) Safari/537.36 Chrome/91.0.4472.164 NeteaseMusicDesktop/3.0.18."
    "203152";
const char *kReferer = "https://music.163.com/";
const char *kFormType = "application/x-www-form-urlencoded";
const int kMaxHops = 6;

bool CheckOk(JNIEnv *env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return true;
}

jstring ToJString(JNIEnv *env, const char *s) {
    jstring out = env->NewStringUTF(s ? s : "");
    if (!CheckOk(env) || !out) return nullptr;
    return out;
}

std::string FromJString(JNIEnv *env, jstring s) {
    if (!s) return std::string();
    const char *chars = env->GetStringUTFChars(s, nullptr);
    if (!CheckOk(env) || !chars) return std::string();
    std::string out(chars);
    env->ReleaseStringUTFChars(s, chars);
    return out;
}

struct Request {
    const char *method = "GET";
    const char *url = nullptr;
    const char *body = nullptr;  // POST only
    const char *cookie = nullptr;
    int connect_ms = 10000;
    int read_ms = 10000;
    size_t max_body = 262144;
    bool header_only = false;  // GET only
};

// POST bodies are not re-sent across redirects: after the first 3xx the
// request continues as a plain GET, matching browser behavior.
int Perform(JavaVM *vm, const Request &req, HttpResponse *out) {
    out->code = -1;
    out->body.clear();
    out->final_url.clear();

    JNIEnv *env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
        JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != 0 || !env) return -1;
        attached = true;
    }

    int result = -1;
    std::string current = req.url;
    bool first_hop = true;

    jclass url_cls = env->FindClass("java/net/URL");
    jclass conn_cls = env->FindClass("java/net/HttpURLConnection");
    jclass stream_cls = env->FindClass("java/io/InputStream");
    jclass out_stream_cls = env->FindClass("java/io/OutputStream");
    if (!CheckOk(env) || !url_cls || !conn_cls || !stream_cls ||
        !out_stream_cls)
        goto done;

    for (int hop = 0; hop <= kMaxHops; ++hop) {
        if (env->PushLocalFrame(64) != 0) break;
        const char *method = (first_hop || req.body == nullptr)
                                 ? req.method
                                 : "GET";
        bool send_body = first_hop && req.body != nullptr;

        jmethodID url_ctor = env->GetMethodID(url_cls, "<init>",
                                              "(Ljava/lang/String;)V");
        jmethodID url_join_ctor = env->GetMethodID(
            url_cls, "<init>", "(Ljava/net/URL;Ljava/lang/String;)V");
        jmethodID open_conn = env->GetMethodID(
            url_cls, "openConnection", "()Ljava/net/URLConnection;");
        jmethodID url_str =
            env->GetMethodID(url_cls, "toString", "()Ljava/lang/String;");
        if (!CheckOk(env) || !url_ctor || !url_join_ctor || !open_conn ||
            !url_str) {
            env->PopLocalFrame(nullptr);
            break;
        }
        jstring jurl = ToJString(env, current.c_str());
        if (!jurl) {
            env->PopLocalFrame(nullptr);
            break;
        }
        jobject url_obj = env->NewObject(url_cls, url_ctor, jurl);
        if (!CheckOk(env) || !url_obj) {
            env->PopLocalFrame(nullptr);
            break;
        }
        jobject conn = env->CallObjectMethod(url_obj, open_conn);
        if (!CheckOk(env) || !conn || !env->IsInstanceOf(conn, conn_cls)) {
            env->PopLocalFrame(nullptr);
            break;
        }
        jmethodID set_method = env->GetMethodID(conn_cls, "setRequestMethod",
                                                "(Ljava/lang/String;)V");
        jmethodID set_prop = env->GetMethodID(
            conn_cls, "setRequestProperty",
            "(Ljava/lang/String;Ljava/lang/String;)V");
        jmethodID set_ctime =
            env->GetMethodID(conn_cls, "setConnectTimeout", "(I)V");
        jmethodID set_rtime =
            env->GetMethodID(conn_cls, "setReadTimeout", "(I)V");
        jmethodID set_follow = env->GetMethodID(
            conn_cls, "setInstanceFollowRedirects", "(Z)V");
        jmethodID set_output =
            env->GetMethodID(conn_cls, "setDoOutput", "(Z)V");
        jmethodID get_output = env->GetMethodID(
            conn_cls, "getOutputStream", "()Ljava/io/OutputStream;");
        jmethodID get_code =
            env->GetMethodID(conn_cls, "getResponseCode", "()I");
        jmethodID get_header = env->GetMethodID(
            conn_cls, "getHeaderField",
            "(Ljava/lang/String;)Ljava/lang/String;");
        jmethodID get_stream = env->GetMethodID(conn_cls, "getInputStream",
                                                "()Ljava/io/InputStream;");
        jmethodID disconnect = env->GetMethodID(conn_cls, "disconnect", "()V");
        if (!CheckOk(env) || !set_method || !set_prop || !set_ctime ||
            !set_rtime || !set_follow || !set_output || !get_output ||
            !get_code || !get_header || !get_stream || !disconnect) {
            env->PopLocalFrame(nullptr);
            break;
        }
        jstring m_method = ToJString(env, method);
        jstring k_ua = ToJString(env, "User-Agent");
        jstring v_ua = ToJString(env, kDesktopUa);
        jstring k_ref = ToJString(env, "Referer");
        jstring v_ref = ToJString(env, kReferer);
        jstring k_loc = ToJString(env, "Location");
        jstring k_cookie = ToJString(env, "Cookie");
        jstring v_cookie =
            req.cookie ? ToJString(env, req.cookie) : nullptr;
        jstring k_type = ToJString(env, "Content-Type");
        jstring v_type = ToJString(env, kFormType);
        if (!m_method || !k_ua || !v_ua || !k_ref || !v_ref || !k_loc ||
            !k_cookie || !k_type || !v_type ||
            (req.cookie && !v_cookie)) {
            env->PopLocalFrame(nullptr);
            break;
        }
        env->CallVoidMethod(conn, set_method, m_method);
        env->CallVoidMethod(conn, set_prop, k_ua, v_ua);
        env->CallVoidMethod(conn, set_prop, k_ref, v_ref);
        if (req.cookie) env->CallVoidMethod(conn, set_prop, k_cookie,
                                            v_cookie);
        if (send_body)
            env->CallVoidMethod(conn, set_prop, k_type, v_type);
        env->CallVoidMethod(conn, set_ctime, req.connect_ms);
        env->CallVoidMethod(conn, set_rtime, req.read_ms);
        env->CallVoidMethod(conn, set_follow, JNI_FALSE);
        if (send_body) env->CallVoidMethod(conn, set_output, JNI_TRUE);
        if (!CheckOk(env)) {
            env->PopLocalFrame(nullptr);
            break;
        }
        if (send_body) {
            jobject out_stream = env->CallObjectMethod(conn, get_output);
            if (!CheckOk(env) || !out_stream) {
                env->PopLocalFrame(nullptr);
                break;
            }
            jmethodID write = env->GetMethodID(out_stream_cls, "write",
                                               "([BII)V");
            jmethodID flush =
                env->GetMethodID(out_stream_cls, "flush", "()V");
            jmethodID close =
                env->GetMethodID(out_stream_cls, "close", "()V");
            if (!CheckOk(env) || !write || !flush || !close) {
                env->PopLocalFrame(nullptr);
                break;
            }
            size_t body_len = std::strlen(req.body);
            jbyteArray jbody = env->NewByteArray(
                static_cast<jsize>(body_len));
            if (!CheckOk(env) || !jbody) {
                env->PopLocalFrame(nullptr);
                break;
            }
            env->SetByteArrayRegion(
                jbody, 0, static_cast<jsize>(body_len),
                reinterpret_cast<const jbyte *>(req.body));
            if (!CheckOk(env)) {
                env->PopLocalFrame(nullptr);
                break;
            }
            env->CallVoidMethod(out_stream, write, jbody, 0,
                                static_cast<jint>(body_len));
            env->CallVoidMethod(out_stream, flush);
            env->CallVoidMethod(out_stream, close);
            if (!CheckOk(env)) {
                env->PopLocalFrame(nullptr);
                break;
            }
        }
        jint code = env->CallIntMethod(conn, get_code);
        if (!CheckOk(env)) {
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            break;
        }
        out->code = static_cast<int>(code);
        first_hop = false;
        if (code >= 300 && code < 400) {
            jstring loc = static_cast<jstring>(
                env->CallObjectMethod(conn, get_header, k_loc));
            bool ok = CheckOk(env);
            std::string target = ok ? FromJString(env, loc) : std::string();
            std::string resolved;
            if (!target.empty()) {
                jobject base_obj = env->NewObject(url_cls, url_ctor, jurl);
                jstring jtarget = ToJString(env, target.c_str());
                jobject joined = (CheckOk(env) && base_obj && jtarget)
                                     ? env->NewObject(url_cls, url_join_ctor,
                                                      base_obj, jtarget)
                                     : nullptr;
                bool jok = CheckOk(env) && joined;
                jstring jjoined = jok ? static_cast<jstring>(
                                            env->CallObjectMethod(joined,
                                                                  url_str))
                                      : nullptr;
                if (jok && CheckOk(env) && jjoined)
                    resolved = FromJString(env, jjoined);
            }
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            if (resolved.empty()) break;
            current = resolved;
            out->final_url = current;
            if (hop == kMaxHops) {
                result = 1;
                break;
            }
            continue;
        }
        if (code != 200) {
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            break;
        }
        out->final_url = current;
        if (req.header_only) {
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            result = 0;
            break;
        }
        jobject stream = env->CallObjectMethod(conn, get_stream);
        if (!CheckOk(env) || !stream) {
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            break;
        }
        jmethodID read = env->GetMethodID(stream_cls, "read", "([B)I");
        jmethodID close = env->GetMethodID(stream_cls, "close", "()V");
        if (!CheckOk(env) || !read || !close) {
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            break;
        }
        jbyteArray chunk = env->NewByteArray(8192);
        if (!CheckOk(env) || !chunk) {
            env->CallVoidMethod(conn, disconnect);
            env->PopLocalFrame(nullptr);
            break;
        }
        bool body_ok = true;
        for (;;) {
            jint n = env->CallIntMethod(stream, read, chunk);
            if (!CheckOk(env)) {
                body_ok = false;
                break;
            }
            if (n < 0) break;
            if (n == 0) continue;
            if (out->body.size() + static_cast<size_t>(n) > req.max_body) {
                body_ok = false;  // over budget: refuse, do not truncate
                break;
            }
            char tmp[8192];
            env->GetByteArrayRegion(chunk, 0, n,
                                    reinterpret_cast<jbyte *>(tmp));
            if (!CheckOk(env)) {
                body_ok = false;
                break;
            }
            out->body.append(tmp, static_cast<size_t>(n));
        }
        env->CallVoidMethod(stream, close);
        env->CallVoidMethod(conn, disconnect);
        env->PopLocalFrame(nullptr);
        if (body_ok) result = 0;
        break;
    }

done:
    if (attached) vm->DetachCurrentThread();
    if (result != 0) out->body.clear();
    return result;
}

}  // namespace

int HttpGet(JavaVM *vm, const char *url, const char *cookie, int connect_ms,
            int read_ms, size_t max_body, bool header_only,
            HttpResponse *out) {
    if (!vm || !url || !out) return -1;
    Request req;
    req.method = "GET";
    req.url = url;
    req.cookie = cookie;
    req.connect_ms = connect_ms;
    req.read_ms = read_ms;
    req.max_body = max_body;
    req.header_only = header_only;
    return Perform(vm, req, out);
}

int HttpPost(JavaVM *vm, const char *url, const char *body,
             const char *cookie, int connect_ms, int read_ms,
             size_t max_body, HttpResponse *out) {
    if (!vm || !url || !body || !out) return -1;
    Request req;
    req.method = "POST";
    req.url = url;
    req.body = body;
    req.cookie = cookie;
    req.connect_ms = connect_ms;
    req.read_ms = read_ms;
    req.max_body = max_body;
    req.header_only = false;
    return Perform(vm, req, out);
}

}  // namespace netease
