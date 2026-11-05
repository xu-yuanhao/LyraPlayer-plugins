// sync_platform.h - Lyra host integration: JNI bridge, pairing, library I/O.
#pragma once
#include "plugin/lyra_plugin.h"
#ifdef __ANDROID__
#include <jni.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include "sync_transfer.h"

// Android platform (IDL 1) resolution status, probed once at bind time.
typedef struct LyraAndroid {
    int available;              // 1 when java_vm + activity + version==1 OK
    int platform_version;
    void *activity_ref;         // global ref while host lives
    void *vm;                   // JNI JavaVM* (cast in sync_platform.c)
    int version_cached;
} LyraAndroid;

// device identity: stable random id persisted next to the task DB
typedef struct SyncIdentity {
    char device_id[33];        // 16 random bytes hex
    char device_name[64];      // Build.MODEL via JNI at bind time (best effort)
    uint8_t psk[SYNC_KEY_SIZE];
} SyncIdentity;

// pairing: QR encodes lyra-sync://<ip>:<port>#<psk-hex32>
// (first release: PSK carried in QR; swap to numeric-compare PAKE later)
int sync_qr_payload(const SyncIdentity *id, const char *ip, const char *port,
                    char *out, int cap);
int sync_qr_parse(const char *payload, char *ip, int ipcap, char *port, int pcap,
                  uint8_t psk[SYNC_KEY_SIZE]);

// library: enumerate local tracks via media context (system.local)
typedef void (*SyncTrackFn)(void *user, const char *uid, const char *title,
                            const char *audio_uri, int64_t duration_us,
                            const char *cover_uri, const char *lyrics_uri,
                            const char *folder);
int sync_enumerate_local(void *media_context_user, const LyraMediaContextV3 *mc,
                         SyncTrackFn fn, void *user);

// read a local track through the host: openReadFd(track_id) -> fd (owned)
int sync_platform_open_read_fd(LyraAndroid *pa, int64_t track_id);
void sync_platform_close_fd(LyraAndroid *pa, int fd);
// refresh the library after imports
int sync_platform_refresh_library(LyraAndroid *pa);
// choose a writable import directory via SAF (chooseScanFolder)
int sync_platform_choose_scan_folder(LyraAndroid *pa, const char *start);
int sync_platform_take_scan_folder(LyraAndroid *pa, char *out, int cap);
int sync_platform_start_library_scan(LyraAndroid *pa);
int sync_platform_app_data_dir(LyraAndroid *pa, char *out, int cap);
void sync_platform_shutdown(LyraAndroid *pa);

#ifdef SYNC_HOST_TEST
// Host-test hooks (integration tests only; absent on device builds)
extern int g_test_probe_ok;
extern const char *g_test_data_root;
extern int (*g_test_open_fd)(int64_t);
extern int g_test_refresh_count;
extern int g_test_identity_fail;   // identity persistence failure mask
#endif
int sync_identity_load(const char *data_dir, SyncIdentity *id);
int sync_platform_probe(const LyraHostV3 *host, LyraAndroid *pa);

#ifdef __cplusplus
}
#endif
