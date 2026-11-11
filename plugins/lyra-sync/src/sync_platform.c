// sync_platform.c - Android IDL 1 JNI bridge + identity + QR pairing.
//
// Contract (docs/plugins/android + reference/android-platform + SDK
// android-platform-v1.json): verify lyraPlatformVersion()==1 before resolving
// any method; resolve exact JNI descriptors from the published IDL; hold the
// Activity only via a global ref created at bind and deleted at shutdown;
// clear exceptions after every JNI call.
#include "sync_platform.h"
#include "sync_crypto.h"
#include "sync_io.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <jni.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SYNC_HOST_TEST
// identity persistence failure injection: bit 0=create, 1=write, 2=fsync,
// 3=rename; tests set the mask, production always 0.
int g_test_identity_fail = 0;
#endif

// ---------- identity ----------
int sync_identity_load(const char *data_dir, SyncIdentity *id) {
    char path[600];
    snprintf(path, sizeof path, "%s/identity.bin", data_dir);
    sync_fd_t fd;
    if (sync_fopen_read(path, &fd) == 0) {
        uint8_t blob[8 + 16 + 32];
        int got = sync_pread(fd, blob, sizeof blob, 0);
        sync_fclose(fd);
        if (got == (int)sizeof blob && memcmp(blob, "LYRAID1", 7) == 0) {
            for (int i = 0; i < 16; i++)
                snprintf(id->device_id + i * 2, 3, "%02x", blob[8 + i]);
            id->device_id[32] = 0;
            memcpy(id->psk, blob + 24, SYNC_KEY_SIZE);
            snprintf(id->device_name, sizeof id->device_name, "Lyra Device");
            return 0;
        }
    }
    // first run: generate (strong entropy; identity creation is security
    // relevant so refuse to proceed with degraded randomness)
    uint8_t dev[16], psk[SYNC_KEY_SIZE];
    if (rand_bytes_strong(dev, sizeof dev) || rand_bytes_strong(psk, sizeof psk)) {
        return -1;  // caller surfaces "no entropy" instead of weak identity
    }
    for (int i = 0; i < 16; i++)
        snprintf(id->device_id + i * 2, 3, "%02x", dev[i]);
    id->device_id[32] = 0;
    memcpy(id->psk, psk, SYNC_KEY_SIZE);
    snprintf(id->device_name, sizeof id->device_name, "Lyra Device");
    uint8_t blob[8 + 16 + 32];
    memcpy(blob, "LYRAID1", 7); blob[7] = '\n';
    memcpy(blob + 8, dev, 16);
    memcpy(blob + 24, psk, SYNC_KEY_SIZE);
    // Persist via temp + fsync + atomic rename; EVERY step checked. A
    // half-written identity is worse than none (a peer would pair against a
    // key that may not survive), so any failure fails the whole load.
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    sync_unlink(tmp);
#ifdef SYNC_HOST_TEST
    if (g_test_identity_fail & 1) return -1;            // create fail
#endif
    if (sync_fopen_write(tmp, &fd)) return -1;          // create failed
#ifdef SYNC_HOST_TEST
    if (g_test_identity_fail & 2) { sync_fclose(fd); sync_unlink(tmp); return -1; }
#endif
    if (sync_pwrite(fd, blob, sizeof blob, 0)) {
        sync_fclose(fd); sync_unlink(tmp); return -1;   // write failed
    }
#ifdef SYNC_HOST_TEST
    if (g_test_identity_fail & 4) { sync_fclose(fd); sync_unlink(tmp); return -1; }
#endif
    if (sync_fsync(fd)) {                               // flush failed
        sync_fclose(fd); sync_unlink(tmp); return -1;
    }
    sync_fclose(fd);
#ifdef SYNC_HOST_TEST
    if (g_test_identity_fail & 8) { sync_unlink(tmp); return -1; }  // rename fail
#endif
    if (sync_rename(tmp, path)) {                       // publish failed
        sync_unlink(tmp); return -1;
    }
    // durability of the parent directory entry (file+dir are distinct
    // objects; without a dir fsync the rename may not survive power loss)
    {
        char dirp[640];
        snprintf(dirp,sizeof dirp,"%s",path);
        char *cut=strrchr(dirp,'/');
        if (cut) { *cut=0;
            int dr=sync_dir_fsync(dirp);
            if (dr==-1) return -1;   // dir fsync failure => not durable
            // dr==-2 (platform cannot open dirs) is best-effort
        }
    }
    return 0;
}

// ---------- QR payload ----------
int sync_qr_payload(const SyncIdentity *id, const char *ip, const char *port,
                    char *out, int cap) {
    char hex[SYNC_KEY_SIZE * 2 + 1];
    for (int i = 0; i < SYNC_KEY_SIZE; i++)
        snprintf(hex + i * 2, 3, "%02x", id->psk[i]);
    return snprintf(out, (size_t)cap, "lyra-sync://%s:%s#%s", ip, port, hex) < cap ? 0 : -1;
}
int sync_qr_parse(const char *payload, char *ip, int ipcap, char *port, int pcap,
                  uint8_t psk[SYNC_KEY_SIZE]) {
    if (strncmp(payload, "lyra-sync://", 12)) return -1;
    const char *host = payload + 12;
    const char *hash = strchr(host, '#');
    if (!hash || hash - host >= 128) return -1;
    char hostport[128];
    size_t n = (size_t)(hash - host);
    if (n >= sizeof hostport) return -1;
    memcpy(hostport, host, n); hostport[n] = 0;
    const char *colon = strrchr(hostport, ':');
    if (!colon) return -1;
    size_t il = (size_t)(colon - hostport);
    if ((int)il >= ipcap) return -1;
    memcpy(ip, hostport, il); ip[il] = 0;
    snprintf(port, (size_t)pcap, "%s", colon + 1);
    for (int i = 0; i < SYNC_KEY_SIZE; i++) {
        unsigned v;
        if (sscanf(hash + 1 + i * 2, "%2x", &v) != 1) return -1;
        psk[i] = (uint8_t)v;
    }
    return 0;
}

#ifdef SYNC_HOST_TEST
// Host-test hooks (never compiled for Android).
int g_test_probe_ok = 0;
const char *g_test_data_root = NULL;
int (*g_test_open_fd)(int64_t) = NULL;
int g_test_refresh_count = 0;
#endif

#ifdef __ANDROID__
static JNIEnv *jni_attach(LyraAndroid *pa) {
    if (!pa || !pa->vm) return NULL;
    JavaVM *vm = (JavaVM *)pa->vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK)
        return env;
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK)
        return env;
    return NULL;
}
#define JNI_GUARD_BEGIN  JNIEnv *env = jni_attach(pa); \
    if (!pa->available || !pa->vm || !pa->activity_ref || !env) return -1;
#define JNI_GUARD_END
#endif

int sync_platform_probe(const LyraHostV3 *host, LyraAndroid *pa) {
    memset(pa, 0, sizeof *pa);
#ifndef __ANDROID__
    (void)host;
    if (g_test_probe_ok) { pa->available=1; pa->version_cached=1; return 0; }
    return -1;
#else
    if (!host->java_vm || !host->android_context) return -1;
    pa->vm = (JavaVM *)host->java_vm;
    JNIEnv *env = jni_attach(pa);
    if (!env) return -1;
    jobject activity = (jobject)host->android_context;
    jclass type = (*env)->GetObjectClass(env, activity);
    jmethodID ver = (*env)->GetMethodID(env, type, "lyraPlatformVersion", "()I");
    if (!ver || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jint v = (*env)->CallIntMethod(env, activity, ver);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    pa->version_cached = v;
    pa->available = (v == 1);
    // global ref so worker threads can reach the Activity facade; released in
    // sync_platform_shutdown (fresh ref per plugin init — never cached across
    // host restarts)
    pa->activity_ref = (*env)->NewGlobalRef(env, activity);
    (*env)->DeleteLocalRef(env, type);
    return pa->available ? 0 : -1;
#endif
}

#ifdef __ANDROID__
// openReadFd(J)I — IDL 1 operation 34.
int sync_platform_open_read_fd(LyraAndroid *pa, int64_t track_id) {
    JNI_GUARD_BEGIN
    jclass type = (*env)->GetObjectClass(env, (jobject)pa->activity_ref);
    jmethodID m = (*env)->GetMethodID(env, type, "openReadFd", "(J)I");
    if (!m || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jint fd = (*env)->CallIntMethod(env, (jobject)pa->activity_ref, m, (jlong)track_id);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); fd = -1; }
    (*env)->DeleteLocalRef(env, type);
    return fd >= 0 ? fd : -1;
}

void sync_platform_close_fd(LyraAndroid *pa, int fd) {
    (void)pa;
    if (fd >= 0) close(fd);
}

// syncLibraryIncremental()I — IDL 1 op 5
int sync_platform_refresh_library(LyraAndroid *pa) {
    JNI_GUARD_BEGIN
    jclass type = (*env)->GetObjectClass(env, (jobject)pa->activity_ref);
    jmethodID m = (*env)->GetMethodID(env, type, "syncLibraryIncremental", "()I");
    if (!m || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jint r = (*env)->CallIntMethod(env, (jobject)pa->activity_ref, m);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    (*env)->DeleteLocalRef(env, type);
    return r;
}

// startLibraryScan()V — IDL 1 op 26 (full rescan after bulk import)
int sync_platform_start_library_scan(LyraAndroid *pa) {
    JNI_GUARD_BEGIN
    jclass type = (*env)->GetObjectClass(env, (jobject)pa->activity_ref);
    jmethodID m = (*env)->GetMethodID(env, type, "startLibraryScan", "()V");
    if (!m || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    (*env)->CallVoidMethod(env, (jobject)pa->activity_ref, m);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, type);
    return 0;
}

// chooseScanFolder(Ljava/lang/String;)V / takeScanFolder()Ljava/lang/String;
int sync_platform_choose_scan_folder(LyraAndroid *pa, const char *start) {
    JNI_GUARD_BEGIN
    jclass type = (*env)->GetObjectClass(env, (jobject)pa->activity_ref);
    jmethodID m = (*env)->GetMethodID(env, type, "chooseScanFolder", "(Ljava/lang/String;)V");
    if (!m || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jstring js = (*env)->NewStringUTF(env, start ? start : "");
    (*env)->CallVoidMethod(env, (jobject)pa->activity_ref, m, js);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, js);
    (*env)->DeleteLocalRef(env, type);
    return 0;
}

int sync_platform_take_scan_folder(LyraAndroid *pa, char *out, int cap) {
    JNI_GUARD_BEGIN
    jclass type = (*env)->GetObjectClass(env, (jobject)pa->activity_ref);
    jmethodID m = (*env)->GetMethodID(env, type, "takeScanFolder", "()Ljava/lang/String;");
    if (!m || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jstring js = (jstring)(*env)->CallObjectMethod(env, (jobject)pa->activity_ref, m);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    int ok = -1;
    if (js) {
        const char *u = (*env)->GetStringUTFChars(env, js, NULL);
        if (u) { snprintf(out, (size_t)cap, "%s", u); ok = 0; (*env)->ReleaseStringUTFChars(env, js, u); }
        (*env)->DeleteLocalRef(env, js);
    }
    (*env)->DeleteLocalRef(env, type);
    return ok;
}

// Activity.getFilesDir().getAbsolutePath() — app-private, always writable.
int sync_platform_app_data_dir(LyraAndroid *pa, char *out, int cap) {
    JNI_GUARD_BEGIN
    jclass type = (*env)->GetObjectClass(env, (jobject)pa->activity_ref);
    jmethodID m = (*env)->GetMethodID(env, type, "getFilesDir", "()Ljava/io/File;");
    if (!m || (*env)->ExceptionCheck(env)) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jobject file = (*env)->CallObjectMethod(env, (jobject)pa->activity_ref, m);
    if ((*env)->ExceptionCheck(env) || !file) {
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, type);
        return -1;
    }
    jclass ftype = (*env)->GetObjectClass(env, file);
    jmethodID gp = (*env)->GetMethodID(env, ftype, "getAbsolutePath", "()Ljava/lang/String;");
    int ok = -1;
    if (gp && !(*env)->ExceptionCheck(env)) {
        jstring js = (jstring)(*env)->CallObjectMethod(env, file, gp);
        if (js && !(*env)->ExceptionCheck(env)) {
            const char *u = (*env)->GetStringUTFChars(env, js, NULL);
            if (u) { snprintf(out, (size_t)cap, "%s", u); ok = 0; (*env)->ReleaseStringUTFChars(env, js, u); }
            (*env)->DeleteLocalRef(env, js);
        }
    }
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, ftype);
    (*env)->DeleteLocalRef(env, file);
    (*env)->DeleteLocalRef(env, type);
    return ok;
}
#elif defined(SYNC_HOST_TEST)
int sync_platform_open_read_fd(LyraAndroid *pa, int64_t track_id) {
    (void)pa; return g_test_open_fd ? g_test_open_fd(track_id) : -1; }
void sync_platform_close_fd(LyraAndroid *pa, int fd) { (void)pa; (void)fd; }
int sync_platform_refresh_library(LyraAndroid *pa) { (void)pa; g_test_refresh_count++; return 0; }
int sync_platform_start_library_scan(LyraAndroid *pa) { (void)pa; return 0; }
int sync_platform_choose_scan_folder(LyraAndroid *pa, const char *start) { (void)pa; (void)start; return -1; }
int sync_platform_take_scan_folder(LyraAndroid *pa, char *out, int cap) { (void)pa; (void)out; (void)cap; return -1; }
int sync_platform_app_data_dir(LyraAndroid *pa, char *out, int cap) {
    (void)pa; if (!g_test_data_root) return -1;
    snprintf(out,(size_t)cap,"%s",g_test_data_root); return 0; }
#else
int sync_platform_open_read_fd(LyraAndroid *pa, int64_t track_id) { (void)pa; (void)track_id; return -1; }
void sync_platform_close_fd(LyraAndroid *pa, int fd) { (void)pa; (void)fd; }
int sync_platform_refresh_library(LyraAndroid *pa) { (void)pa; return -1; }
int sync_platform_start_library_scan(LyraAndroid *pa) { (void)pa; return -1; }
int sync_platform_choose_scan_folder(LyraAndroid *pa, const char *start) { (void)pa; (void)start; return -1; }
int sync_platform_take_scan_folder(LyraAndroid *pa, char *out, int cap) { (void)pa; (void)out; (void)cap; return -1; }
int sync_platform_app_data_dir(LyraAndroid *pa, char *out, int cap) { (void)pa; (void)out; (void)cap; return -1; }
#endif

void sync_platform_shutdown(LyraAndroid *pa) {
#ifdef __ANDROID__
    if (pa && pa->vm && pa->activity_ref) {
        JNIEnv *env = jni_attach(pa);
        if (env) (*env)->DeleteGlobalRef(env, (jobject)pa->activity_ref);
        pa->activity_ref = NULL;
        pa->available = 0;
    }
#endif
}

#ifdef __cplusplus
}
#endif
