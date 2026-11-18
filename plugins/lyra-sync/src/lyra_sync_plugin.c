// lyra_sync_plugin.c - plugin entry: manifest ABI3 frame plugin "chun.lyra.sync".
#include "plugin/lyra_plugin.h"
#include "sync_platform.h"
#include "sync_common.h"
#include "sync_crypto.h"
#include "sync_io.h"
#include "sync_db.h"
#include "sync_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define CAP_NAME "sync.lyra"
#define PLUGIN_ID "chun.lyra.sync"
#define SCAN_PORT "47521"
#define MAX_SYNC_PLAN_FILES 256
// list layout constants (draw and hit-test MUST use the same values)
#define LIST_ROWS    5
#define LIST_ROW_H   44.f
#define LIST_TOP_PAD 126.f
#define LIST_SCROLL_STEP 5

enum { UI_HOME=0, UI_SENDING, UI_RECEIVE_WAIT, UI_RECEIVING, UI_RESULT,
       UI_WAITING_BATCH };
enum { CONFLICT_ASK=0, CONFLICT_SKIP=1, CONFLICT_OVERWRITE=2, CONFLICT_RENAME=3 };

typedef struct UiState {
    int screen;
    int px_w, px_h;
    SyncProgress progress;
    char toast[160];
    uint64_t toast_until_ms;
    char listen_ip[64], listen_port[16];
    char qr[512];
    char result_summary[288];
    int enum_active;
    uint64_t enum_req;
    int list_top;
    int dest_pending;
} UiState;

typedef struct SyncCtx {
    const LyraHostV3 *host;
    const LyraServiceContextV3 *svc;
    uint64_t cap_handle;
    uint64_t root_handle;
    LyraAndroid android;
    SyncIdentity identity;
    char data_dir[512];
    char dest_dir[512];
    TrackInfo *tracks;
    int track_count, track_cap;
    int initialized;
    UiState ui;
    SyncSession *session;
    SyncManifest manifest;
    int session_role;
    uint64_t task_seq;
#ifdef _WIN32
    HANDLE worker;
    CRITICAL_SECTION lock;
    volatile LONG worker_quit;
#else
    pthread_t worker;
    pthread_mutex_t lock;
    volatile int worker_quit;
#endif
    int worker_running;
    sync_fd_t listen_fd;
    volatile sync_fd_t pending_rx_fd;
    int receiving_enabled;
    int job_send_pending;
    char job_ip[64], job_port[16];
    uint8_t job_psk[SYNC_KEY_SIZE];
    int job_count;
    TrackInfo *job_tracks;
    TrackInfo *batch_queue;
    int batch_total, batch_sent, batch_done, batch_skipped;
    char batch_peer[64];
    int build_abort;
    char pair_accept[8][33];
    int pair_accept_count;
    char pair_pending[33];
    uint64_t pair_confirm_until_ms;
    int conflict_policy;
    int conflicts_skipped;
    int conflicts_renamed;
    int gesture_owner;
    int panel_visible;
} SyncCtx;

static SyncCtx g_ctx;
static SyncCtx *g_commit_ctx;
static sync_fd_t g_src_fd[MAX_SYNC_PLAN_FILES];
static int g_src_count;

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID arg);
#else
static void *worker_main(void *arg);
#endif
// batch state persistence (defined later)
static void batch_state_path(const SyncCtx *c, char *out, size_t cap);
static int batch_state_save(SyncCtx *c);
// persisted batch identity: the packed manifest is saved alongside the task
// so a process restart restores the SAME manifest bytes (same id) as the
// interrupted session; the receiver temp blocks stay addressable.
static SyncManifest batch_manifest;   // persisted batch identity (id+crcs)
static SyncFileMeta *batch_files=NULL;// owned by batch_manifest
static int batch_have=0;              // 1 = manifest restored/built
static int batch_m2s[MAX_SYNC_PLAN_FILES];      // manifest i -> queue i
static int64_t batch_tid[MAX_SYNC_PLAN_FILES];  // manifest i -> source track id
static int batch_build_skipped=0;
static uint8_t *batch_pk=NULL;        // snapshot of the packed manifest bytes
static size_t batch_pkn=0;            // packed length
#ifdef SYNC_HOST_TEST
// test accessors: plant/clear a synthetic snapshot for save/load scenarios
void test_batch_pk_set(uint8_t *pk, size_t pkn) {
    free(batch_pk);
    batch_pk=pk; batch_pkn=pkn;
}
#endif
static void batch_state_clear(SyncCtx *c);
static void batch_state_load(SyncCtx *c);
static void enum_start(SyncCtx *c);
static void enum_poll(SyncCtx *c);
static void eng_close_sources(void);
static void start_worker(SyncCtx *c);

// ---- locking primitives ----
#ifdef _WIN32
static CRITICAL_SECTION g_lock;
static void lock_init(SyncCtx *c) { InitializeCriticalSection(&g_lock); }
static void lock_take(SyncCtx *c) { EnterCriticalSection(&g_lock); }
static void lock_give(SyncCtx *c) { LeaveCriticalSection(&g_lock); }
static void lock_destroy(SyncCtx *c) { DeleteCriticalSection(&g_lock); }
#else
static pthread_mutex_t g_lock;
static void lock_init(SyncCtx *c) { pthread_mutex_init(&g_lock,NULL); }
static void lock_take(SyncCtx *c) { pthread_mutex_lock(&g_lock); }
static void lock_give(SyncCtx *c) { pthread_mutex_unlock(&g_lock); }
static void lock_destroy(SyncCtx *c) { pthread_mutex_destroy(&g_lock); }
#endif

static void toast_locked(SyncCtx *c, const char *msg) {
    snprintf(c->ui.toast, sizeof c->ui.toast, "%s", msg);
    c->ui.toast_until_ms = sync_now_ms() + 4000;
}
#define toast(c,msg) do { lock_take(c); toast_locked(c,msg); lock_give(c); } while(0)

static int cap_request(void *user, const LyraMessageV3 *msg, LyraReplyV3 *reply);

// ---- receive mode & send helpers ----
static void start_receive(SyncCtx *c) {
    if (c->listen_fd==SYNC_FD_INVALID) {
        if (sync_tcp_listen(SCAN_PORT,&c->listen_fd)) {
            c->listen_fd=SYNC_FD_INVALID;
            toast_locked(c,"listen failed: port busy");
            return;
        }
        snprintf(c->ui.listen_port,sizeof c->ui.listen_port,"%s",SCAN_PORT);
        if (sync_local_ip(c->ui.listen_ip,(int)sizeof c->ui.listen_ip)) {
            snprintf(c->ui.listen_ip,sizeof c->ui.listen_ip,"%s","unavailable");
            c->ui.qr[0]=0;
            toast_locked(c,"cannot determine device IP: connect Wi-Fi first");
        } else {
            sync_qr_payload(&c->identity,c->ui.listen_ip,c->ui.listen_port,
                            c->ui.qr,sizeof c->ui.qr);
            if (strstr(c->ui.qr,"unavailable")) c->ui.qr[0]=0;
        }
    }
    c->receiving_enabled=1;
    c->ui.screen=UI_RECEIVE_WAIT;
    start_worker(c);
}
static void start_send(SyncCtx *c) {
    LyraMessageV3 m; memset(&m,0,sizeof m);
    m.size=sizeof m; m.operation=9;
    int rc=cap_request(NULL,&m,NULL);
    if (rc==LYRA_MESSAGE_OK) return;
    if (rc==LYRA_MESSAGE_UNSUPPORTED)
        toast_locked(c,"no tracks selected or peer settings empty");
    else if (rc==LYRA_MESSAGE_INVALID)
        toast_locked(c,"peer settings invalid (check address/key)");
    else
        toast_locked(c,"send failed");
}
// v0.2.3: media contract fix (item/collection_state return 0 on success per
// public docs — the v0.2.2 ==1 check broke enumeration on the real host),
// sender builds the manifest BEFORE connecting (large libraries no longer
// race the receiver's manifest wait), single lock/unlock in cap_request op9,
// commit-stage same-name conflict policy (never silently overwrite), commit
// durability (fsync temp + parent dir before rename), pairing-confirm gating
// on the receiver, and section()-based non-takeover UI when the host offers
// a capability region.
#include "plugin/lyra_plugin.h"
#include "sync_platform.h"
#include "sync_common.h"
#include "sync_crypto.h"
#include "sync_io.h"
#include "sync_db.h"
#include "sync_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#define CAP_NAME "sync.lyra"
// list layout constants (draw and hit-test MUST use the same values)
#define LIST_ROWS    5      // rows per page
#define LIST_ROW_H   44.f   // dp per row
#define LIST_TOP_PAD 126.f  // dp from panel top to first row (chips above)
#define LIST_SCROLL_STEP 5  // tracks per edge-tap (<= LIST_ROWS)
#define PLUGIN_ID "chun.lyra.sync"
#define SCAN_PORT "47521"
#define MAX_TRACKS 10000
#define MAX_SYNC_PLAN_FILES 256



// commit policy for a destination file that already exists (same rel_path).
// Default is ASK: never overwrite user music silently. The "overwrite"
// choice must arrive explicitly via op11 for this task, or globally via
// config; without either, the file is kept as (name (1).ext).






// ---- receiver commit helpers (restored v0.3.4) ----
// file SHA-256 over the whole content (publish verification + conflict
// detection); returns 0 ok, -1 open/alloc failure.
static int sync_sha256_file(const char *path, uint8_t out[32]) {
    sync_fd_t fd;
    if (sync_fopen_read(path,&fd)) return -1;
    sha256_ctx hc; sha256_init(&hc);
    uint8_t *b=(uint8_t*)malloc(SYNC_CHUNK_BYTES);
    int ok=-1;
    if (b) {
        ok=0;
        uint64_t off=0;
        for (;;) {
            int got=sync_pread(fd,b,SYNC_CHUNK_BYTES,off);
            if (got<0) { ok=-1; break; }
            if (got==0) break;
            sha256_update(&hc,b,(size_t)got);
            off+=(uint64_t)got;
        }
        sha256_final(&hc,out);
        free(b);
    }
    sync_fclose(fd);
    return ok;
}
static int path_exists(const char *p) {
    sync_fd_t fd;
    if (sync_fopen_read(p,&fd)) return 0;
    sync_fclose(fd);
    return 1;
}
// receiver commit: engine provides temp path + metadata; destination keeps
// the original rel_path (filename+extension) under the authorized dir.
// Conflicts: an existing destination with different content is never
// overwritten silently - policy decides skip/rename-new/overwrite. Returns
// 0 = published; SYNC_COMMIT_SKIP = incoming content intentionally skipped
// (conflict policy), never an error.
static int eng_commit_file(void *user, const SyncCommitInfo *ci) {
    SyncCtx *c=(SyncCtx*)user;
    char dst[768];
    const char *dest = c->dest_dir[0] ? c->dest_dir : c->data_dir;
    if (!ci->meta->rel_path[0] || strstr(ci->meta->rel_path,"..")) return -1;
    snprintf(dst,sizeof dst,"%s/%s",dest,ci->meta->rel_path);
    char dirpart[768]; snprintf(dirpart,sizeof dirpart,"%s",dst);
    char *cut=strrchr(dirpart,47);
    if (cut) {
        *cut=0;
        if (sync_mkdirs(dirpart)) return -1;   // destination dir unusable
    }
    if (path_exists(dst)) {
        // same-name destination: identical content is a no-op success; a
        // differing one goes through the conflict policy (never silent).
        uint8_t sha_dst[32];
        if (sync_sha256_file(dst,sha_dst)==0 && !memcmp(sha_dst,ci->meta->sha,32)) {
            sync_unlink(ci->temp_path);
            return 0;
        }
        int policy=c->conflict_policy;
        if (policy==CONFLICT_SKIP) {
            // keep the existing file; caller answers SKIP, not COMMIT_OK
            c->conflicts_skipped++;
            sync_unlink(ci->temp_path);
            return SYNC_COMMIT_SKIP;
        }
        if (policy==CONFLICT_OVERWRITE) { /* fall through to publish */ }
        else {
            // ASK and RENAME both keep the old file: publish under a
            // non-colliding "name (n).ext" variant so nothing is lost.
            char alt[768];
            const char *ext=strrchr(ci->meta->rel_path,'.');
            for (int n=1;n<1000;n++) {
                if (ext && ext!=ci->meta->rel_path)
                    snprintf(alt,sizeof alt,"%s/%.*s (%d)%s",dest,
                             (int)(ext-ci->meta->rel_path),ci->meta->rel_path,n,ext);
                else
                    snprintf(alt,sizeof alt,"%s/%s (%d)",dest,ci->meta->rel_path,n);
                if (!path_exists(alt)) break;
            }
            snprintf(dst,sizeof dst,"%s",alt);
            c->conflicts_renamed++;
        }
    }
    // publish pipeline (v0.2.4): never write the final name in place.
    // 1) copy engine temp -> "<dst>.lytmp" in the DESTINATION tree
    //    (rename only works atomically within one filesystem; engine temp
    //    lives in app data, dest may be another volume);
    // 2) SHA-256 the .lytmp and compare with the manifest sha (re-verifies
    //    what will be published after the copy, catching copy corruption);
    // 3) fsync the .lytmp; 4) atomic rename onto dst; 5) fsync the dir.
    // Every step checks its error and leaves the previous destination
    // untouched on failure.
    char pub[820];
    snprintf(pub,sizeof pub,"%s.lytmp",dst);
    sync_unlink(pub);   // stale .lytmp from an earlier crash
    {
        sync_fd_t sfd;
        if (sync_fopen_read(ci->temp_path,&sfd)) return -1;
        uint8_t *b=(uint8_t*)malloc(SYNC_CHUNK_BYTES);
        int ok=0;
        if (b) {
            sync_fd_t dfd;
            if (sync_fopen_write(pub,&dfd)==0) {
                ok=1;
                uint64_t off=0;
                for (;;) {
                    int got=sync_pread(sfd,b,SYNC_CHUNK_BYTES,off);
                    if (got<0) { ok=0; break; }
                    if (got==0) break;
                    if (sync_pwrite(dfd,b,(size_t)got,off)) { ok=0; break; }
                    off+=(uint64_t)got;
                }
                uint64_t pub_sz=0;
                // every durability step checked: flush failure or a size
                // mismatch after the copy fails the publish
                if (ok && sync_fsync(dfd)) ok=0;
                if (ok && sync_file_size(pub,&pub_sz)) ok=0;
                sync_fclose(dfd);
                if (!ok) sync_unlink(pub);
            } else {
                ok=0;
            }
            free(b);
        }
        sync_fclose(sfd);
        if (!ok) return -1;
    }
    {
        uint8_t sha_pub[32];
        if (sync_sha256_file(pub,sha_pub) || memcmp(sha_pub,ci->meta->sha,32)) {
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "publish verify failed: %s",ci->meta->rel_path);
            sync_unlink(pub);
            return -1;
        }
    }
    if (sync_rename(pub,dst)) { sync_unlink(pub); return -1; }
    {
        // directory entry durability via sync_dir_fsync: -1 = open/fsync
        // failed on a platform that CAN fsync dirs (the published name may
        // not survive power loss => report failure); -2 = platform cannot
        // open dirs (best effort, e.g. Windows) => not an error.
        char dp[768]; snprintf(dp,sizeof dp,"%s",dst);
        char *dc=strrchr(dp,47);
        if (dc) {
            *dc=0;
            if (sync_dir_fsync(dp)==-1) return -1;
        }
    }
    sync_unlink(ci->temp_path);
    sync_platform_refresh_library(&c->android);
    return 0;
}
// test hook (SYNC_HOST_TEST builds): routes receiver commits through a
// test-installed function so the REAL worker path is exercised; production
// always calls eng_commit_file directly.
#ifdef SYNC_HOST_TEST
int (*g_test_commit_hook)(void*, const SyncCommitInfo*) = NULL;
#endif
static int eng_commit_dispatch(void *user, const SyncCommitInfo *ci) {
#ifdef SYNC_HOST_TEST
    if (g_test_commit_hook) return g_test_commit_hook(user,ci);
#endif
    return eng_commit_file(user,ci);
}

// ---- pairing config schema (v0.2.8) ----
static const char *CFG_TABLE = "sync.lyra";
static const char *CFG_IP    = "sync.lyra.peer_ip";
static const char *CFG_PORT  = "sync.lyra.peer_port";
static const char *CFG_PSK   = "sync.lyra.peer_psk";
static int cfg_schema_count(void *ud) { (void)ud; return 3; }
static int cfg_schema_describe(void *ud, int32_t i, LyraParameterV3 *p) {
    (void)ud;
    if (!p || p->size!=sizeof(*p) || i<0 || i>2) return -1;
    memset(p,0,sizeof *p);
    p->size=sizeof *p;
    p->schema_version=1;
    switch (i) {
    case 0: p->kind=0; p->flags=0;
            p->key=CFG_IP; p->name="Peer address";
            p->description="IPv4/IPv6 address of the receiving device";
            p->group="Pairing";
            p->default_value="\"192.168.1.100\"";
            break;
    case 1: p->kind=1;
            p->flags=LYRA_CONFIG_INTEGER|LYRA_CONFIG_MINIMUM|LYRA_CONFIG_MAXIMUM;
            p->minimum=1; p->maximum=65535;
            p->key=CFG_PORT; p->name="Peer port";
            p->description="Sync listen port of the receiving device";
            p->group="Pairing";
            p->default_value="47521";
            break;
    case 2: p->kind=0; p->flags=0;
            p->key=CFG_PSK; p->name="Pairing key (PSK hex)";
            p->description="64 hex chars shown on the receiving device";
            p->group="Pairing";
            p->default_value="\"\"";
            break;
    }
    return 0;
}
static int cfg_schema_text(void *ud, const char *key, int32_t field,
                           char *out, int32_t cap) {
    (void)ud;
    if (!key || !out || cap<=0) return -1;
    const char *v=NULL;
    if (field==0) v="";
    else if (field==1) v = !strcmp(key,CFG_PORT) ? "47521" : "";
    else if (field==2) v = !strcmp(key,CFG_IP) ? "Peer address"
                        : !strcmp(key,CFG_PORT) ? "Peer port"
                        : "Pairing key (PSK hex)";
    else if (field==3) v = !strcmp(key,CFG_IP) ? "IPv4/IPv6 address of the receiving device"
                        : !strcmp(key,CFG_PORT) ? "Sync listen port of the receiving device"
                        : "64 hex chars shown on the receiving device";
    else if (field==5) v = "Pairing";
    if (!v) return -1;
    int need=(int)strlen(v)+1;
    if (need>cap) return need;
    memcpy(out,v,(size_t)need);
    return need;
}
static int cfg_read_pairing(SyncCtx *c, char *ip, int ipc,
                            char *port, int pc, char *pskhex, int pc2) {
    if (!c->host || !c->host->query_interface) return -1;
    const LyraConfigSchemaV3 *hcfg=(const LyraConfigSchemaV3 *)
        c->host->query_interface(c->host->user,LYRA_INTERFACE_CONFIG_SCHEMA,
                                 LYRA_INTERFACE_VERSION,sizeof(LyraConfigSchemaV3));
    if (!hcfg || !hcfg->text) return -1;
    int r1=hcfg->text(hcfg->user,CFG_IP,0,ip,ipc);
    if (r1<=0 || r1>ipc) return -1;
    int r2=hcfg->text(hcfg->user,CFG_PORT,0,port,pc);
    if (r2<=0 || r2>pc) return -1;
    int r3=hcfg->text(hcfg->user,CFG_PSK,0,pskhex,pc2);
    if (r3<=0 || r3>pc2) return -1;
    return 0;
}
static const LyraConfigSchemaV3 g_cfg_schema={
    sizeof(LyraConfigSchemaV3),LYRA_INTERFACE_VERSION,NULL,
    cfg_schema_count,cfg_schema_describe,cfg_schema_text};

// ---- worker thread ----
static int eng_read_source(void *user, int fi, uint64_t off, void *buf, size_t cap) {
    (void)user;
    if (fi<0 || fi>=g_src_count) return -1;
    if (g_src_fd[fi]==SYNC_FD_INVALID) return -1;
    return sync_pread(g_src_fd[fi],buf,cap,off);
}
static void eng_close_sources(void) {
    for (int i=0;i<MAX_SYNC_PLAN_FILES;i++)
        if (g_src_fd[i]!=SYNC_FD_INVALID) { sync_fclose(g_src_fd[i]); g_src_fd[i]=SYNC_FD_INVALID; }
    g_src_count=0;
}
static void session_cleanup_locked(SyncCtx *c) {
    if (c->session) { sync_session_destroy(c->session); c->session=NULL; }
    c->session_role=0;
    eng_close_sources();
}
static void start_worker(SyncCtx *c) {
    if (c->worker_running) return;
#ifdef _WIN32
    InterlockedExchange(&c->worker_quit,0);
    c->worker=CreateThread(NULL,0,worker_main,c,0,NULL);
#else
    __atomic_store_n(&c->worker_quit,0,__ATOMIC_SEQ_CST);
    pthread_create(&c->worker,NULL,worker_main,c);
#endif
    c->worker_running=1;
}
static void stop_worker(SyncCtx *c) {
    if (!c->worker_running) return;
    lock_take(c);
    if (c->session) {
        sync_session_cancel(c->session);
        sync_fd_t sfd=sync_session_fd(c->session);
        if (sfd!=SYNC_FD_INVALID) sync_shutdown_fd(sfd);
    }
    if (c->listen_fd!=SYNC_FD_INVALID) sync_shutdown_fd(c->listen_fd);
    if (c->pending_rx_fd!=SYNC_FD_INVALID) sync_shutdown_fd(c->pending_rx_fd);
    lock_give(c);
#ifdef _WIN32
    InterlockedExchange(&c->worker_quit,1);
    if (c->worker) { WaitForSingleObject(c->worker,10000); CloseHandle(c->worker); c->worker=NULL; }
#else
    __atomic_store_n(&c->worker_quit,1,__ATOMIC_SEQ_CST);
    pthread_join(c->worker,NULL);
#endif
    c->worker_running=0;
}



// worker quit helpers (lock-free; worker_quit is Interlocked/atomic)
static int worker_should_quit(SyncCtx *c) {
#ifdef _WIN32
    return InterlockedCompareExchange(&c->worker_quit,0,0)!=0;
#else
    return __atomic_load_n(&c->worker_quit,__ATOMIC_SEQ_CST)!=0;
#endif
}
static void set_worker_quit(SyncCtx *c) {
#ifdef _WIN32
    InterlockedExchange(&c->worker_quit,1);
#else
    __atomic_store_n(&c->worker_quit,1,__ATOMIC_SEQ_CST);
#endif
}

#define REDIAL_ATTEMPTS 900   // ~30 min of 2s backoff before giving up
#define REDIAL_SLEEP_MS 2000

// ---- sender: run the whole batched send task (with auto-redial) ----
// Unified batch recovery model:
//   batch_queue[0..batch_total)  ALL selected files for the task (source of
//                                truth, kept until the task completes)
//   batch_sent                   cursor: entries [0..batch_sent) are handled
//   batch_done/batch_skipped     independent accounting counters
// The current batch slice is [batch_sent .. min(batch_sent+256, total)).
// After each committed file the state file is rewritten (checked write +
// fsync + rename + dir fsync), so a process crash resumes at the exact
// cursor. connect/attach/transfer failures rebuild everything from the
// queue and redial with backoff - the task is only dropped when the whole
// queue is handled or the operator cancels.
static void sender_run_batches(SyncCtx *c, const char *ip, const char *port,
                               const uint8_t *psk) {
    uint8_t key[SYNC_KEY_SIZE];
    memcpy(key,psk,SYNC_KEY_SIZE);
    int fail_run=0;                   // consecutive interrupted sessions
    // Batch-manifest cache: lives at FILE SCOPE (global) so a process
    // restart can adopt the manifest persisted by batch_state_load —
    // rebuilding it here would shadow the restored identity and give the
    // resume session a DIFFERENT manifest id (orphaning every receiver
    // temp block). This function only mutates it when a NEW batch begins.
    int resume_at=0;                      // manifest cursor for the next session
    int batch_build_skipped=0;
    for (;;) {
        if (worker_should_quit(c)) return;
        lock_take(c);
        int total=c->batch_total, sent=c->batch_sent;
        int have_queue=c->batch_queue!=NULL;
        lock_give(c);
        if (!have_queue || sent>=total) return;   // nothing left to send
        // resumed task: track ids are re-resolved against the library, so
        // the library must be enumerated first. enum_poll runs on the frame
        // thread (host on_frame), which keeps pumping while we wait.
        lock_take(c);
        int need_enum=(c->track_count==0);
        lock_give(c);
        if (need_enum) {
            enum_start(c);
            for (int w=0;w<100 && c->ui.enum_active;w++) {
                if (worker_should_quit(c)) break;
                sync_sleep_ms(100);
            }
            lock_take(c);
            int got_lib=c->track_count>0;
            lock_give(c);
            if (!got_lib) {
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                         "paused: library not ready - state kept, resumes on next start");
                c->ui.screen=UI_RESULT;
                return;
            }
        }
        int slice_n=total-sent;
        if (slice_n>MAX_SYNC_PLAN_FILES) slice_n=MAX_SYNC_PLAN_FILES;
        // rebuild the slice from the queue; allocation failure is retryable,
        // it never drops the task
        TrackInfo *slice=NULL;
        for (int tries=0;tries<50 && !slice;tries++) {
            slice=(TrackInfo*)calloc((size_t)slice_n,sizeof(TrackInfo));
            if (!slice) sync_sleep_ms(200);
        }
        if (!slice) {
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "paused: out of memory");
            c->ui.screen=UI_RESULT;
            return;                       // still resumable from batch_state
        }
        lock_take(c);
        free(c->job_tracks);
        c->job_tracks=slice;
        memcpy(c->job_tracks,c->batch_queue+sent,(size_t)slice_n*sizeof(TrackInfo));
        c->job_count=slice_n;
        c->build_abort=0;
        // resume correctness: entries restored from batch_state carry uid
        // only; re-resolve track ids against the current library enum.
        for (int i=0;i<c->job_count;i++) {
            if (c->job_tracks[i].track_id) continue;
            for (int j=0;j<c->track_count;j++)
                if (c->tracks[j].uid[0] &&
                    !strcmp(c->tracks[j].uid,c->job_tracks[i].uid)) {
                    c->job_tracks[i].track_id=c->tracks[j].track_id;
                    break;
                }
        }
        lock_give(c);

#ifdef SYNC_HOST_TEST
        fprintf(stderr,"[RM] batch pass: have=%d count=%u\n",batch_have,batch_manifest.count);
#endif
        // ---- manifest build FIRST (only on the first pass of this batch;
        // redials reuse batch_manifest so the manifest id stays stable and
        // receiver temp paths keep their blocks).
        int done0,skip0;
        int build_first=0;
        // m2s/build_skipped alias the batch-lifetime cache (valid while
        // batch_have=1; recomputed on rebuild)
        int *m2s=batch_m2s;
        if (!batch_have) {
        batch_build_skipped=0;
        build_first=1;
        manifest_free(&c->manifest);
        memset(&c->manifest,0,sizeof c->manifest);
        // manifest index -> slice index. Stack-local (redeclared per batch
        // loop pass), but valid on redials via the cached batch manifest —
        // the mapping is deterministic for a given batch (P1-a note in
        // docs/RECOVERY-MODEL.md).
        lock_take(c);
        int count=c->job_count;
        done0=c->batch_done; skip0=c->batch_skipped;
        c->manifest.files=(SyncFileMeta*)calloc(count?count:1,sizeof(SyncFileMeta));
        lock_give(c);
        int out=0; uint64_t total_bytes=0;
        uint8_t *mbuf=(uint8_t*)malloc(SYNC_BLOCK);
        for (int i=0;i<count && mbuf && !c->build_abort;i++) {
            lock_take(c);
            int64_t tid=c->job_tracks[i].track_id;
            char rel[SYNC_MAX_NAME+1];
            snprintf(rel,sizeof rel,"%s",c->job_tracks[i].fname);
            lock_give(c);
            sync_fd_t sfd=sync_platform_open_read_fd(&c->android,tid);
            if (sfd==SYNC_FD_INVALID || rel[0]==0) { batch_build_skipped++; continue; }
            batch_tid[out]=tid;   // cache the REAL source track id for redials
            SyncFileMeta *f=&c->manifest.files[out];
            snprintf(f->rel_path,sizeof f->rel_path,"%s",rel);
            uint64_t lo=0, hi=1;
            for (;;) {
                uint8_t t1;
                if (sync_pread(sfd,&t1,1,hi-1)!=1) break;
                lo=hi; hi*=2;
                if (hi>(1ull<<40)) break;
            }
            while (lo+1<hi) {
                uint64_t mid=lo+(hi-lo)/2;
                uint8_t t1;
                if (sync_pread(sfd,&t1,1,mid-1)==1) lo=mid; else hi=mid;
            }
            f->size=lo;
            uint64_t nb_max=(f->size+SYNC_BLOCK-1)/SYNC_BLOCK;
            if (nb_max>SYNC_MAX_FILE_BLOCKS) { sync_fclose(sfd); continue; }
            f->block_crcs = nb_max? (uint32_t*)malloc((size_t)nb_max*4) : NULL;
            sha256_ctx hc; sha256_init(&hc);
            uint64_t sz=0, nb=0;
            for (;;) {
                if (c->build_abort) break;
                int got=sync_pread(sfd,mbuf,SYNC_BLOCK,sz);
                if (got<=0) break;
                sha256_update(&hc,mbuf,(size_t)got);
                if (f->block_crcs && nb<SYNC_MAX_FILE_BLOCKS)
                    f->block_crcs[nb++]=sync_crc32c(0,mbuf,(size_t)got);
                sz+=(uint64_t)got;
                if (got<(int)SYNC_BLOCK) break;
            }
            sha256_final(&hc,f->sha);
            total_bytes+=sz; f->kind=FK_AUDIO;
            m2s[out]=i;
            out++;
            g_src_fd[out-1]=(sync_fd_t)sfd;
            g_src_count=out;
        }
        free(mbuf);
        c->manifest.count=(uint32_t)out;
        c->manifest.total_bytes=total_bytes;
        if (out==0 || c->build_abort) {
            batch_have=0;   // cache invalid: rebuild on the next pass
            eng_close_sources();
            manifest_free(&c->manifest); manifest_free(&batch_manifest); memset(&batch_manifest,0,sizeof batch_manifest); batch_files=NULL; batch_have=0;
            memset(&c->manifest,0,sizeof c->manifest);
            if (c->build_abort) {
                // user cancel while building: drop the whole task
                lock_take(c);
                c->build_abort=0;
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,"canceled");
                c->ui.screen=UI_RESULT;
                batch_state_clear(c);
                free(batch_pk); batch_pk=NULL; batch_pkn=0;
                free(c->batch_queue); c->batch_queue=NULL; c->batch_total=0;
                free(c->job_tracks); c->job_tracks=NULL; c->job_count=0;
                lock_give(c);
                return;
            }
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "no readable tracks in batch");
            c->ui.screen=UI_RESULT;
            // drop the task: every entry was tried and none readable
            lock_take(c);
            free(c->batch_queue); c->batch_queue=NULL; c->batch_total=0;
            free(c->job_tracks); c->job_tracks=NULL; c->job_count=0;
            batch_state_clear(c);
            lock_give(c);
            return;
        }
        // successful build: cache a deep copy as the STABLE batch manifest,
        // then hand ownership of the CRC arrays to the cache (c->manifest
        // borrows them for this pass; each redial re-borrows).
        // The manifest id must be computed NOW (manifest_pack normally does
        // it lazily at first pump); caching before that would leave id=0 in
        // the cache and give redials a DIFFERENT task_id — orphaning every
        // receiver temp block (root cause of the round-18 merge corruption).
        {
            uint8_t *pk=NULL; size_t pkn=0;
            if (manifest_pack(&c->manifest,&pk,&pkn)) {
                eng_close_sources();
                manifest_free(&c->manifest); manifest_free(&batch_manifest); memset(&batch_manifest,0,sizeof batch_manifest); batch_files=NULL; batch_have=0;
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                         "manifest pack failed");
                c->ui.screen=UI_RESULT;
                return;
            }
            // SNAPSHOT for batch_state_save: packed bytes are frozen here,
            // never re-packed inside the pump-loop lock (per-file saves)
            free(batch_pk);
            batch_pk=pk; batch_pkn=pkn;
        }
        batch_files=c->manifest.files;
        batch_manifest=c->manifest;
        batch_have=1;
        }  // end of build_first guard
        else {
#ifdef SYNC_HOST_TEST
            fprintf(stderr,"[RM] redial/adopt path: have=%d pk=%p pkn=%zu count=%u\n",
                    batch_have,(void*)batch_pk,batch_pkn,batch_manifest.count);
#endif
            // redial: restore c->manifest from the stable cache (same id!)
            c->manifest=batch_manifest;
            // source fds were closed with the previous session: reopen them
            // for the cached manifest entries (build_first fills them during
            // the hash pass; here we only need read handles)
            eng_close_sources();
            for (int i=0;i<(int)c->manifest.count && i<MAX_SYNC_PLAN_FILES;i++) {
                // redial/restart reopens by resolving the manifest
                // rel_path against the library (uid match is deterministic
                // and content-bound). batch_tid is preferred when it was
                // captured by this process's own build.
                int64_t tid=0;
                if (batch_tid[i]) tid=batch_tid[i];
                else {
                    lock_take(c);
                    for (int j=0;j<c->track_count;j++)
                        if (c->tracks[j].uid[0] &&
                            !strcmp(c->tracks[j].fname,c->manifest.files[i].rel_path)) {
                            tid=c->tracks[j].track_id; break;
                        }
                    lock_give(c);
                }
                sync_fd_t sfd=sync_platform_open_read_fd(&c->android,tid);
                if (sfd==SYNC_FD_INVALID) {
                    // a source became unreadable between sessions: rebuild is
                    // the only safe path (P1-b: this changes the manifest!)
                    batch_have=0; batch_files=NULL;
                    memset(&batch_manifest,0,sizeof batch_manifest);
                    break;
                }
                g_src_fd[i]=sfd; g_src_count=i+1;
            }
        }

        // ---- connect with backoff (auto-redial for recoverable unreachability)
        sync_fd_t fd=SYNC_FD_INVALID;
        int attempt=0;
        for (;;) {
            if (worker_should_quit(c) || c->build_abort) break;
            if (sync_tcp_connect(ip,port,&fd,8000)==0) break;
            attempt++;
            if (attempt>REDIAL_ATTEMPTS) break;
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "reconnecting to %s:%s (attempt %d) - batch %d/%d",
                     ip,port,attempt,total,sent);
            // abortable backoff (same cancel semantics as the post-WAIT wait)
            int aborted=0;
            for (int ms=0; ms<REDIAL_SLEEP_MS && !aborted; ms+=100) {
                if (worker_should_quit(c)) { aborted=2; break; }
                lock_take(c);
                int ba=c->build_abort;
                lock_give(c);
                if (ba) aborted=1;
                else sync_sleep_ms(100);
            }
            if (aborted==1) break;    // cancel: handled below
        }
        if (worker_should_quit(c)) { eng_close_sources();
            memset(&c->manifest,0,sizeof c->manifest);   // borrow released; cache keeps arrays
            return; }
        if (fd==SYNC_FD_INVALID) {
            eng_close_sources();
            memset(&c->manifest,0,sizeof c->manifest);   // borrow released; cache keeps arrays
            if (c->build_abort) {
                lock_take(c);
                c->build_abort=0;
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,"canceled");
                c->ui.screen=UI_RESULT;
                batch_state_clear(c);
                free(c->batch_queue); c->batch_queue=NULL; c->batch_total=0;
                free(c->job_tracks); c->job_tracks=NULL; c->job_count=0;
                lock_give(c);
                return;
            }
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "connect %s:%s failed - state kept, retry by sending again",
                     ip,port);
            c->ui.screen=UI_RESULT;
            return;                       // state file keeps the resume cursor
        }

        // ---- attach. Handshake failure means wrong key or protocol
        // mismatch: terminal, redialing cannot fix it.
        SyncEngine seng={eng_read_source,NULL,eng_commit_dispatch,c};
        SyncSession *ns=sync_session_create(&seng,c->data_dir,1,key);
        sync_session_set_own_device(ns,c->identity.device_id);
        if (sync_session_attach(ns,fd)) {
            SyncProgress pr; sync_session_get_progress(ns,&pr);
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "failed: %s (auth/protocol) - state kept for corrected retry",
                     pr.error);
            c->ui.screen=UI_RESULT;
            sync_session_destroy(ns);
            eng_close_sources();
            memset(&c->manifest,0,sizeof c->manifest);   // borrow released; cache keeps arrays
            lock_take(c);
            if (batch_state_save(c))
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                         "failed: %s (auth/protocol); WARNING: resume point NOT saved (%s)",
                         pr.error,strerror(errno));
            else
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                         "failed: %s (auth/protocol) - state kept for corrected retry",
                         pr.error);
            lock_give(c);
            c->ui.screen=UI_RESULT;
            return;
        }
        // resume with the STABLE manifest: same id -> same task_id ->
        // receiver temp paths keep their transferred blocks. The sender
        // starts from manifest entry 0: for already-transferred files the
        // receiver's HAVE_BITMAP marks every block valid, the sender sends
        // nothing, the receiver re-verifies and answers COMMIT_OK (publish
        // is an idempotent no-op) — cursor alignment is implicit in the
        // protocol, no side-channel needed.
        (void)resume_at;
        sync_sender_start(ns,&c->manifest);
        lock_take(c);
        c->session=ns; c->session_role=1;
        c->task_seq++;
        c->ui.screen=UI_SENDING;
        lock_give(c);

        // pump to completion; persist per-file progress for crash resume.
        // The per-file cursor maps through m2s so files dropped at build
        // time (unreadable) never skew the resume position.
        int last_files=-1;
        for (;;) {
            if (worker_should_quit(c)) { sync_session_cancel(ns); }
            int r=sync_session_pump(ns,100);
            lock_take(c);
            if (c->session==ns) {
                SyncProgress pr; sync_session_get_progress(ns,&pr);
                c->ui.progress=pr;
                int resolved=pr.files_done+pr.files_skipped;
                if (resolved!=last_files) {
                    last_files=resolved;
                    int adv = resolved>0 ? m2s[resolved-1]+1 : 0;
                    resume_at=adv;
                    c->batch_sent=sent+adv;
                    c->batch_done=done0+pr.files_done;
                    c->batch_skipped=skip0+pr.files_skipped;
                    if (batch_state_save(c))
                        snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                                 "WARNING: resume point NOT saved (%s) - transfer continues",
                                 strerror(errno));
                }
            }
            lock_give(c);
            if (r!=0) break;
        }
        int st=sync_session_state(ns);
        lock_take(c);
        SyncProgress pr; sync_session_get_progress(ns,&pr);
        c->ui.progress=pr;
        session_cleanup_locked(c);        // closes session + source fds
        int resolved=pr.files_done+pr.files_skipped;
        int adv = resolved>0 ? m2s[resolved-1]+1 : 0;
        if (st==TS_DONE) adv=slice_n;     // whole slice handled at DONE
        resume_at=adv;
        c->batch_sent=sent+adv;
        c->batch_done=done0+pr.files_done;
        c->batch_skipped=skip0+pr.files_skipped;
        int sent_now=c->batch_sent;
        lock_give(c);
        memset(&c->manifest,0,sizeof c->manifest);   // borrow released; cache keeps arrays
        if (worker_should_quit(c)) return;   // shutdown: state already saved

        if (st==TS_DONE) {
            if (sent_now>=total) {
                // source-read failures must be visible, never silent
                // (P1-b in docs/RECOVERY-MODEL.md)
                if (batch_build_skipped)
                    snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                             "done: %d committed, %d skipped; %d UNREADABLE source file(s) NOT sent",
                             c->batch_done,c->batch_skipped,batch_build_skipped);
                else
                    snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                             "done: %d file(s) committed, %d skipped (batched)",
                             c->batch_done,c->batch_skipped);
                c->ui.screen=UI_RESULT;
                lock_take(c);
                batch_state_clear(c);
                free(c->batch_queue); c->batch_queue=NULL;
                free(c->job_tracks); c->job_tracks=NULL; c->job_count=0;
                lock_give(c);
                return;                   // whole task complete
            }
            if (batch_state_save(c))      // batch boundary: persist, then redial
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                         "WARNING: resume point NOT saved (%s) - redialing anyway",
                         strerror(errno));
            // next batch: rebuild (new slice) and start at its first entry
            batch_have=0; batch_files=NULL;
            memset(&batch_manifest,0,sizeof batch_manifest);
            free(batch_pk); batch_pk=NULL; batch_pkn=0;
            resume_at=0;
            continue;
        }
        if (st==TS_CANCELED) {
            // user cancel ends the task and drops the resume state
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,"canceled");
            c->ui.screen=UI_RESULT;
            lock_take(c);
            batch_state_clear(c);
            free(c->batch_queue); c->batch_queue=NULL;
            free(c->job_tracks); c->job_tracks=NULL; c->job_count=0;
            lock_give(c);
            return;
        }
        if (st==TS_FAILED) {
            // auth/protocol failure is terminal: no redial with the same
            // parameters; the state file stays for a corrected retry
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "failed: %s (auth/protocol) - state kept for corrected retry",
                     pr.error);
            c->ui.screen=UI_RESULT;
            return;
        }
        // TS_WAITING: recoverable disconnect - keep the task and redial.
        fail_run++;
        snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                 "reconnecting after interruption: %s",pr.error);
        if (fail_run>=60) {
            fail_run=0;
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "paused: %s - state kept for resume",pr.error);
            c->ui.screen=UI_RESULT;
            return;
        }
        // abortable backoff: op7 (cancel) sets build_abort; a cancel during
        // the redial sleep must stop the task NOW, not after the sleep.
        {
            int aborted=0;
            for (int ms=0; ms<REDIAL_SLEEP_MS && !aborted; ms+=100) {
                if (worker_should_quit(c)) { aborted=2; break; }
                lock_take(c);
                int ba=c->build_abort;
                lock_give(c);
                if (ba) aborted=1;
                else sync_sleep_ms(100);
            }
            if (aborted) {
                lock_take(c);
                c->build_abort=0;
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,"canceled");
                c->ui.screen=UI_RESULT;
                batch_state_clear(c);
                free(c->batch_queue); c->batch_queue=NULL; c->batch_total=0;
                free(c->job_tracks); c->job_tracks=NULL; c->job_count=0;
                lock_give(c);
                return;
            }
        }
    }
}

// ---- worker thread: single loop serving send tasks and receive mode ----
#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID arg)
#else
static void *worker_main(void *arg)
#endif
{
    SyncCtx *c=(SyncCtx*)arg;
    for (;;) {
        if (worker_should_quit(c)) break;
        lock_take(c);
        int have_job=c->job_send_pending;
        SyncSession *s=c->session;
        lock_give(c);
        if (have_job) {
            lock_take(c);
            c->job_send_pending=0;
            char ip[64],port[16];
            snprintf(ip,sizeof ip,"%s",c->job_ip);
            snprintf(port,sizeof port,"%s",c->job_port);
            // credential ref: the peer PSK this task was authorized with
            // (op9 field, or restored from batch_state). NEVER substitute
            // this device's own identity key: resuming with the wrong
            // credential must fail closed, not open a new pairing.
            uint8_t psk[SYNC_KEY_SIZE];
            memcpy(psk,c->job_psk,SYNC_KEY_SIZE);
            int psk_set=0;
            for (int i=0;i<SYNC_KEY_SIZE;i++) if (psk[i]) { psk_set=1; break; }
            lock_give(c);
            if (!psk_set) {
                snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                         "paused: peer credential missing - resend with correct settings");
                c->ui.screen=UI_RESULT;
                sync_sleep_ms(1000);
                continue;
            }
            sender_run_batches(c,ip,port,psk);
            continue;
        }
        if (s) {
            int r=sync_session_pump(s,100);
            if (r!=0) {
                lock_take(c);
                int st=sync_session_state(s);
                SyncProgress pr; sync_session_get_progress(s,&pr);
                c->ui.progress=pr;
                session_cleanup_locked(c);
                lock_give(c);
                if (st==TS_DONE)
                    snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                             "done: %d file(s) committed and verified",pr.files_done);
                else if (st==TS_FAILED)
                    snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                             "failed: %s",pr.error);
                else
                    snprintf(c->ui.result_summary,sizeof c->ui.result_summary,"canceled");
                c->ui.screen=UI_RESULT;
            } else {
                lock_take(c);
                if (c->session) {
                    SyncProgress pr; sync_session_get_progress(c->session,&pr);
                    c->ui.progress=pr;
                }
                lock_give(c);
            }
        } else if (c->receiving_enabled && c->listen_fd!=SYNC_FD_INVALID) {
            fd_set rd; struct timeval tv; tv.tv_sec=0; tv.tv_usec=200*1000;
            FD_ZERO(&rd);
#ifdef _WIN32
            FD_SET((SOCKET)c->listen_fd,&rd);
            if (select((int)c->listen_fd+1,&rd,NULL,NULL,&tv)<=0) continue;
#else
            FD_SET(c->listen_fd,&rd);
            if (select(c->listen_fd+1,&rd,NULL,NULL,&tv)<=0) continue;
#endif
            sync_fd_t cfd; char rip[64];
            if (sync_tcp_accept(c->listen_fd,&cfd,rip,64)==0) {
                sync_settimeo(cfd,500,500);
                SyncEngine eng={NULL,NULL,eng_commit_dispatch,c};
                SyncSession *ns=sync_session_create(&eng,c->data_dir,0,c->identity.psk);
                c->pending_rx_fd=cfd;
                sync_session_set_own_device(ns,c->identity.device_id);
                sync_receiver_start(ns);
                int ar=sync_session_attach(ns,cfd);
                c->pending_rx_fd=SYNC_FD_INVALID;
                if (ar) {
                    sync_session_destroy(ns);
                    toast(c,"pairing failed: key mismatch");
                    continue;
                }
                // pairing-confirm gate: a first-time peer (device id not in
                // the accept list) parks here. The PSK already proved the
                // operator typed the same key; the gate keeps a leaked key
                // from silently starting a bulk copy. Confirmed peers
                // transfer directly. Deadline is sync_now_ms()-based (the
                // test virtual clock advances it); while waiting we only
                // watch for peer hangup - no session pump, so no manifest
                // is accepted before the user decides.
                const char *peer=sync_session_peer_device(ns);
                lock_take(c);
                int known=0;
                for (int i=0;i<c->pair_accept_count;i++)
                    if (!strcmp(c->pair_accept[i],peer)) { known=1; break; }
                if (known) {
                    c->session=ns; c->session_role=2;
                    c->ui.screen=UI_RECEIVING;
                    lock_give(c);
                } else {
                    snprintf(c->pair_pending,sizeof c->pair_pending,"%s",peer);
                    c->pair_confirm_until_ms=sync_now_ms()+60000;
                    lock_give(c);
                    toast(c,"pairing request: confirm on this device");
                    int timed_out=0;
                    // attach left a 90s socket timeout (gate margin). A
                    // blocking peek here would park the whole loop on a
                    // quiet socket for that long, so poll with a short
                    // window instead and restore the margin afterwards.
                    sync_settimeo(cfd, 500, 150);
                    for (int w=0;w<600;w++) {
                        if (worker_should_quit(c)) break;
                        lock_take(c);
                        int pend=c->pair_pending[0];
                        int decided=(pend==0 || pend=='n');
                        lock_give(c);
                        if (decided) break;
                        if (sync_now_ms()>c->pair_confirm_until_ms) { timed_out=1; break; }
                        if (sync_recv_peek(cfd)==0) break;   // peer hung up
                        sync_sleep_ms(100);
                    }
                    sync_settimeo(cfd, 90000, 90000);   // restore attach margin
                    lock_take(c);
                    int confirmed=(c->pair_pending[0]==0) && !timed_out;
                    c->pair_pending[0]=0;
                    if (confirmed && c->pair_accept_count<8 && !worker_should_quit(c)) {
                        snprintf(c->pair_accept[c->pair_accept_count],
                                 sizeof c->pair_accept[0],"%s",peer);
                        c->pair_accept_count++;
                        c->session=ns; c->session_role=2;
                        c->ui.screen=UI_RECEIVING;
                        lock_give(c);
                    } else {
                        lock_give(c);
                        sync_session_destroy(ns);
                        toast(c,timed_out?"pairing timed out":"pairing rejected");
                        continue;
                    }
                }
            }
        } else {
            sync_sleep_ms(50);
        }
        sync_sleep_ms(10);
    }
    lock_take(c);
    session_cleanup_locked(c);
    lock_give(c);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

// ---- batch state persistence (v0.3.4) ----
static void batch_state_path(const SyncCtx *c, char *out, size_t cap) {
    snprintf(out,cap,"%s/batch_state.txt",c->data_dir);
}
// escape TAB/NL/CR in a field so the line format stays parseable
static void batch_state_escape(const char *in, char *out, size_t cap) {
    // '!' is self-escaped so the wire format is unambiguous:
    //   TAB/NL/CR -> "!#"   literal '!' -> "!!"
    size_t o=0;
    for (; *in && o+2<cap; in++) {
        unsigned char ch=(unsigned char)*in;
        if (ch=='!') { out[o++]='!'; out[o++]='!'; }
        else if (ch==0x09 || ch==0x0A || ch==0x0D) { out[o++]='!'; out[o++]='#'; }
        else out[o++]=ch;
    }
    out[o]=0;
}
static void batch_state_unescape(const char *in, char *out, size_t cap) {
    // inverse of batch_state_escape: advance ONE input char per output char;
    // the second byte of an escape pair is skipped explicitly
    size_t o=0;
    for (; *in && o+1<cap; in++) {
        if (in[0]=='!' && in[1]=='#') { out[o++]=0x09; in++; }
        else if (in[0]=='!' && in[1]=='!') { out[o++]='!'; in++; }
        else out[o++]=*in;
    }
    out[o]=0;
}
// persist: remaining queue + independent counters + the PEER credential the
// task was authorized with (hex). Returns 0 ok, -1 fail.
// The credential line is REQUIRED: a resumed task must redial with the same
// peer credential the user entered — never with this device's own identity
// key. The file lives in app-private storage next to identity.bin, written
// with the same checked temp+fsync+rename+dir-fsync pipeline.
static int batch_state_save(SyncCtx *c) {
    if (!c->batch_queue) return -1;
    char path[600],tmp[640];
    batch_state_path(c,path,sizeof path);
    snprintf(tmp,sizeof tmp,"%s.tmp",path);
    sync_unlink(tmp);
    sync_fd_t fd;
    if (sync_fopen_write(tmp,&fd)) return -1;
    char pskhex[SYNC_KEY_SIZE*2+1];
    for (int i=0;i<SYNC_KEY_SIZE;i++)
        snprintf(pskhex+i*2,3,"%02x",c->job_psk[i]);
    char hdr[256];
    int w=snprintf(hdr,sizeof hdr,"%d\t%d\t%d\t%d\n%s\n%s\n%s\n",
                   c->batch_total,c->batch_sent,c->batch_done,c->batch_skipped,
                   c->job_ip,c->job_port,pskhex);
    if (w<=0 || (size_t)w>=sizeof hdr) { sync_fclose(fd); return -1; }
    if (sync_pwrite(fd,hdr,(size_t)w,0)) { sync_fclose(fd); return -1; }
    uint64_t off=(uint64_t)w;
    for (int i=c->batch_sent;i<c->batch_total;i++) {
        char t_esc[192],f_esc[160];
        batch_state_escape(c->batch_queue[i].title,t_esc,sizeof t_esc);
        batch_state_escape(c->batch_queue[i].fname,f_esc,sizeof f_esc);
        char line[SYNC_MAX_NAME+400];
        int w2=snprintf(line,sizeof line,"%s\t%s\t%s\n",
                        c->batch_queue[i].uid,t_esc,f_esc);
        if (w2<=0 || (size_t)w2>=sizeof line) { sync_fclose(fd); return -1; }
        if (sync_pwrite(fd,line,(size_t)w2,off)) { sync_fclose(fd); return -1; }
        off+=(uint64_t)w2;
    }
    // Uses the SNAPSHOT packed at build time (batch_pk) -- packing here,
    // on every per-file save, would run manifest_pack inside the pump-loop
    // lock while the session is live (crash reproduced in lib9 L2).
    if (batch_pk && batch_pkn) {
        size_t need=batch_pkn*2+4;
        char *mline=(char*)malloc(need);
        if (mline) {
            size_t o=0;
            mline[o++]='M'; mline[o++]=' ';
            static const char *hx="0123456789abcdef";
            for (size_t i=0;i<batch_pkn;i++) {
                mline[o++]=hx[batch_pk[i]>>4];
                mline[o++]=hx[batch_pk[i]&15];
            }
            mline[o++]='\n';
            if (sync_pwrite(fd,mline,o,off)) { free(mline);
                sync_fclose(fd); return -1; }
            off+=o;
            free(mline);
        }
    }
    if (sync_fsync(fd)) { sync_fclose(fd); return -1; }   // flush failed
    sync_fclose(fd);
    if (sync_rename(tmp,path)) { sync_unlink(tmp); return -1; }
    // parent dir fsync (rename durability)
    char dirp[640]; snprintf(dirp,sizeof dirp,"%s",path);
    char *cut=strrchr(dirp,'/');
    if (cut) { *cut=0;
        if (sync_dir_fsync(dirp)==-1) return -1;
    }
    return 0;
}
static void batch_state_clear(SyncCtx *c) {
    char path[600];
    batch_state_path(c,path,sizeof path);
    sync_unlink(path);
}
// restore on init: rebuild the send task from batch_state.txt. The file
// stores the queue entries from batch_sent onward plus independent
// counters, so restore reconstructs total/sent/done/skipped exactly and
// zeroes the already-sent head (never re-sent). PSK is not persisted: the
// resume redials with this device's own identity PSK - the same credential
// that performed the first pairing. Any malformed line fails the whole
// restore (strict validation: a half-readable state must not produce a
// half-continued task).
static void batch_state_load(SyncCtx *c) {
    // Parse EVERYTHING into local objects; hand over to the context in ONE
    // block at the end, only after every check passed. Ownership while
    // parsing: q (queue), rm (manifest), pk (packed bytes), buf (file text).
    // buf stays alive for the whole parse (the M-line search reads it); the
    // handover frees exactly the objects that were NOT adopted.
    char path[600];
    batch_state_path(c,path,sizeof path);
    sync_fd_t fd;
    if (sync_fopen_read(path,&fd)) return;
    uint64_t fsz=0;
    sync_file_size(path,&fsz);
    if (fsz<=0 || fsz>16*1024*1024) { sync_fclose(fd); return; }
    char *buf=(char*)malloc((size_t)fsz+1);
    if (!buf) { sync_fclose(fd); return; }
    int got=sync_pread(fd,buf,(size_t)fsz,0);
    sync_fclose(fd);
    if (got<=0) { free(buf); return; }
    buf[got]=0;

    // ---- local objects under construction ----
    TrackInfo *q=NULL;            // batch queue (total entries)
    SyncManifest rm; memset(&rm,0,sizeof rm);   // persisted manifest
    int rm_valid=0;               // manifest unpacked successfully
    size_t bn=0;               // packed byte length
    uint8_t *pk=NULL;             // packed manifest bytes (freed after unpack)
    int fail=0;

    // ---- header ----
    int total=0,sent=0,done=0,skipped=0;
    char peer[64]={0},port[16]={0},pskhex[SYNC_KEY_SIZE*2+1]={0};
    if (sscanf(buf,"%d\t%d\t%d\t%d\n%63[^\n]\n%15[^\n]\n%64[^\n]\n",
               &total,&sent,&done,&skipped,peer,port,pskhex)!=7) fail=1;
    char *p=buf;
    if (!fail) {
        for (int l=0;l<4 && *p; l++) { while (*p && *p!=0x0A) p++; if (*p==0x0A) p++; }
        if (total<=0 || total>MAX_SYNC_PLAN_FILES*8 || sent<0 || sent>total ||
            !peer[0] || !port[0]) fail=1;
    }
    // ---- peer credential (REQUIRED) ----
    uint8_t rpsk[SYNC_KEY_SIZE];
    if (!fail) {
        int psk_ok = strlen(pskhex)==SYNC_KEY_SIZE*2;
        for (int i=0; psk_ok && i<SYNC_KEY_SIZE; i++) {
            unsigned x;
            if (sscanf(pskhex+i*2,"%2x",&x)!=1) { psk_ok=0; break; }
            rpsk[i]=(uint8_t)x;
        }
        if (!psk_ok) fail=1;
    }
    // ---- queue entries ----
    if (!fail) {
        q=(TrackInfo*)calloc((size_t)total,sizeof(TrackInfo));
        if (!q) fail=1;
    }
    if (!fail) {
        for (int i=sent;i<total && !fail;i++) {
            char uidb[64],tb[192],fb[160];
            if (sscanf(p,"%63[^\t]\t%191[^\t]\t%159[^\n]\n",uidb,tb,fb)!=3) { fail=1; break; }
            batch_state_unescape(tb,q[i].title,sizeof q[i].title);
            batch_state_unescape(fb,q[i].fname,sizeof q[i].fname);
            snprintf(q[i].uid,sizeof q[i].uid,"%s",uidb);
            q[i].track_id=0;   // uid-based; re-resolved from the library before send
            while (*p && *p!=0x0A) p++;
            if (*p==0x0A) p++;
        }
    }
    // ---- persisted batch manifest (M line) ----
    // The packed manifest bytes are the batch identity: without them the
    // rebuild would produce a DIFFERENT manifest id and orphan every
    // receiver temp block. Old format (no M line): refuse the resume but
    // keep the state file (the task stays recoverable by a manual resend
    // with the same selection).
    const char *m=strstr(buf,"\nM ");
    if (!fail && !m) fail=1;
    if (!fail) {
        m+=3;
        size_t hexlen=strcspn(m,"\n");
        // validate hex characters and even length; the manifest has
        // variable-length file names and per-block CRCs, so the length is
        // NOT a fixed modulus -- only evenness and minimum size are checkable
        if (hexlen<66 || hexlen%2) fail=1;
        if (!fail) {
            for (size_t i=0;i<hexlen && !fail;i++) {
                char ch=m[i];
                if (!((ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F')))
                    fail=1;
            }
        }
        if (!fail) {
            bn=hexlen/2;
            pk=(uint8_t*)malloc(bn?bn:1);
            if (!pk) fail=1;
            for (size_t i=0;!fail && i<bn;i++) {
                unsigned x;
                if (sscanf(m+i*2,"%2x",&x)!=1) { fail=1; break; }
                pk[i]=(uint8_t)x;
            }
            if (!fail) {
                if (manifest_unpack(pk,bn,&rm) || rm.count==0 ||
                    rm.count>MAX_SYNC_PLAN_FILES) fail=1;
                else rm_valid=1;
            }
        }
    }
    // ---- one-shot handover (only after every check passed) ----
    if (!fail && rm_valid) {
        free(c->batch_queue);
        c->batch_queue=q; q=NULL;          // ownership moved
        c->batch_total=total;
        c->batch_sent=sent;
        c->batch_done=done;
        c->batch_skipped=skipped;
        snprintf(c->job_ip,sizeof c->job_ip,"%s",peer);
        snprintf(c->job_port,sizeof c->job_port,"%s",port);
        memcpy(c->job_psk,rpsk,SYNC_KEY_SIZE);
        manifest_free(&batch_manifest);    // release any previous cache
        batch_manifest=rm;                 // owns rm.files now
        memset(&rm,0,sizeof rm);           // ownership moved
        batch_files=batch_manifest.files;
        // adopt the packed bytes as the batch snapshot: batch_state_save
        // writes the M line from batch_pk; without it the post-resume saves
        // would strip the identity from the state file
        free(batch_pk);
        batch_pk=pk; batch_pkn=bn;
        pk=NULL;
        batch_have=1;
        // Mapping: the persisted manifest was built by THIS task's build
        // pass walking the queue in order (skipping only unreadable
        // entries, which are absent from the packed bytes). The un-sent
        // tail entries (queue[sent..total)) are all present, so manifest
        // entry (count - (total-sent) + k) corresponds to queue entry
        // (sent + k). Validate by rel_path for every tail entry.
        if ((int)batch_manifest.count < total-sent) { fail=1; }
        else {
            int head = (int)batch_manifest.count - (total-sent);
            for (int k=0;(uint32_t)(head+k)<batch_manifest.count && !fail;k++) {
                batch_m2s[head+k]=sent+k;
                if (strcmp(batch_manifest.files[head+k].rel_path,
                           c->batch_queue[sent+k].fname)) { fail=1; break; }
            }
        }
    }
    // ---- cleanup: release only what was NOT handed over ----
    if (q) free(q);
    if (rm_valid) manifest_free(&rm);     // only when NOT adopted
    if (pk) free(pk);
    free(buf);
    if (fail) return;                     // context untouched on failure
    c->job_send_pending=1;                // resume: redial when the worker runs
}

// ---------- library enumeration (frame thread, bounded per frame) -------
// Media contract (docs/plugins/media, fixed v0.2.2): collection_state/item
// return 0 on success, count returns -1 when the request is invalid.
static void enum_poll(SyncCtx *c) {
    if (!c->ui.enum_active) return;
    const LyraMediaContextV3 *mc = (const LyraMediaContextV3 *)
        c->host->query_interface(c->host->user,LYRA_INTERFACE_MEDIA_CONTEXT,
                                 LYRA_INTERFACE_VERSION,sizeof(LyraMediaContextV3));
    if (!mc) { c->ui.enum_active=0; toast(c,"host has no media context"); return; }
    LyraMediaCollectionStateV3 cs; memset(&cs,0,sizeof cs);
    cs.size=sizeof cs;
    if (mc->collection_state(mc->user,c->ui.enum_req,&cs)!=0) return;
    int n=mc->count(mc->user,c->ui.enum_req);
    if (n<0) return;                        // invalid request: wait, do not clobber
    if (n>MAX_TRACKS) n=MAX_TRACKS;
    // grow first; clamp to capacity only after growth
    while (c->track_cap<n) {
        int newcap=c->track_cap? c->track_cap*2 : 64;
        if (newcap>MAX_TRACKS) newcap=MAX_TRACKS;
        TrackInfo *nt=(TrackInfo*)realloc(c->tracks,(size_t)newcap*sizeof(TrackInfo));
        if (!nt) { c->ui.enum_active=0; toast(c,"oom"); return; }
        c->tracks=nt; c->track_cap=newcap;
    }
    for (int i=c->track_count;i<n;i++) {
        LyraMediaDescriptorV3 d; memset(&d,0,sizeof d);
        d.size=sizeof d;
        if (mc->item(mc->user,c->ui.enum_req,i,&d)!=0) continue;
        TrackInfo *t=&c->tracks[i];
        memset(t,0,sizeof *t);
        snprintf(t->uid,sizeof t->uid,"%s",d.uid?d.uid:"");
        snprintf(t->title,sizeof t->title,"%s",d.title?d.title:"(untitled)");
        if (d.audio_uri && d.audio_uri[0]) {
            const char *base=strrchr(d.audio_uri,47);
            base=base?base+1:d.audio_uri;
            snprintf(t->fname,sizeof t->fname,"%s",base);
        }
        if (!t->fname[0])
            snprintf(t->fname,sizeof t->fname,"%s.flac",t->title);
        char *q=strchr(t->fname,63); if (q) *q=0;
        t->track_id=strtoll(t->uid,NULL,LYRA_MEDIA_DECIMAL_BASE);
        t->selected=0;
        c->track_count=i+1;
    }
    if (cs.complete || c->track_count>=MAX_TRACKS) {
        c->ui.enum_active=0;
        mc->cancel(mc->user,c->ui.enum_req);
        c->ui.enum_req=0;
    }
}
static void enum_start(SyncCtx *c) {
    const LyraMediaContextV3 *mc = (const LyraMediaContextV3 *)
        c->host->query_interface(c->host->user,LYRA_INTERFACE_MEDIA_CONTEXT,
                                 LYRA_INTERFACE_VERSION,sizeof(LyraMediaContextV3));
    if (!mc) { toast(c,"host has no media context"); return; }
    c->track_count=0;
    c->ui.enum_req=mc->request(mc->user,"system.local",LYRA_MEDIA_COLLECTION,
                               "system.local","","",NULL);
    if (!c->ui.enum_req) { toast(c,"library request rejected"); return; }
    c->ui.enum_active=1;
}

// ---------- capability request handler ----------
static int cap_request(void *user, const LyraMessageV3 *msg, LyraReplyV3 *reply) {
    (void)user;
    if (!msg || msg->size!=sizeof(*msg)) return LYRA_MESSAGE_INVALID;
    SyncCtx *c=&g_ctx;
    static int64_t stv; static const char *detail="Lyra Player sync plugin";
    switch (msg->operation) {
    case 1: {
        if (!reply || reply->capacity < 1) return LYRA_MESSAGE_CAPACITY;
        static const int64_t one=1;
        reply->fields[0]=(LyraFieldV3){1,LYRA_FIELD_I64,0,sizeof(one),&one};
        reply->field_count=1;
        return LYRA_MESSAGE_OK; }
    case 2: {
        if (!reply || reply->capacity < 2) return LYRA_MESSAGE_CAPACITY;
        lock_take(c);
        stv=c->session?sync_session_state(c->session):TS_IDLE;
        lock_give(c);
        reply->fields[0]=(LyraFieldV3){1,LYRA_FIELD_I64,0,sizeof(stv),&stv};
        reply->fields[1]=(LyraFieldV3){2,LYRA_FIELD_UTF8,0,(uint32_t)strlen(detail)+1,detail};
        reply->field_count=2;
        return LYRA_MESSAGE_OK; }
    case 3: {
        start_receive(c);
        if (!reply || reply->capacity < 2) return LYRA_MESSAGE_CAPACITY;
        reply->fields[0]=(LyraFieldV3){1,LYRA_FIELD_UTF8,0,(uint32_t)strlen(c->ui.listen_ip)+1,c->ui.listen_ip};
        reply->fields[1]=(LyraFieldV3){2,LYRA_FIELD_UTF8,0,(uint32_t)strlen(c->ui.listen_port)+1,c->ui.listen_port};
        reply->field_count=2;
        return LYRA_MESSAGE_OK; }
    case 4:
        c->receiving_enabled=0;
        if (c->listen_fd!=SYNC_FD_INVALID) {
            sync_shutdown_fd(c->listen_fd);
            sync_close(c->listen_fd);
            c->listen_fd=SYNC_FD_INVALID;
        }
        if (c->pending_rx_fd!=SYNC_FD_INVALID) sync_shutdown_fd(c->pending_rx_fd);
        return LYRA_MESSAGE_OK;
    case 5: {
        if (!reply || reply->capacity < 1) return LYRA_MESSAGE_CAPACITY;
        reply->fields[0]=(LyraFieldV3){1,LYRA_FIELD_UTF8,0,(uint32_t)strlen(c->ui.qr)+1,c->ui.qr};
        reply->field_count=1;
        return LYRA_MESSAGE_OK; }
    case 6:
        if (sync_platform_choose_scan_folder(&c->android,"Download"))
            return LYRA_MESSAGE_UNSUPPORTED;
        c->ui.dest_pending=1;
        return LYRA_MESSAGE_OK;
    case 10: {
        static char out[512];   // reply borrows until next frame
        if (sync_platform_take_scan_folder(&c->android,out,sizeof out)) {
            c->ui.dest_pending=0;
            return LYRA_MESSAGE_UNSUPPORTED;
        }
        c->ui.dest_pending=0;
        // SAF pick may surface a content:// URI rather than a filesystem
        // path. POSIX file APIs cannot write through it and the platform IDL
        // exposes no URI-write op, so refuse it explicitly instead of
        // committing into a path that will fail (v0.2.2 stored it verbatim).
        if (!strncmp(out,"content://",10)) {
            snprintf(c->ui.result_summary,sizeof c->ui.result_summary,
                     "picked folder is a SAF URI (content://); this host "
                     "build exposes no URI write op - falling back to app "
                     "private storage");
            c->ui.screen=UI_RESULT;
            toast(c,"SAF URI not writable by plugin; using app storage");
            return LYRA_MESSAGE_UNSUPPORTED;
        }
        snprintf(c->dest_dir,sizeof c->dest_dir,"%s",out);
        if (!reply || reply->capacity < 1) return LYRA_MESSAGE_CAPACITY;
        reply->fields[0]=(LyraFieldV3){1,LYRA_FIELD_UTF8,0,(uint32_t)strlen(out)+1,out};
        reply->field_count=1;
        return LYRA_MESSAGE_OK; }
    case 7:
        lock_take(c);
        if (c->session) {
            sync_session_cancel(c->session);
            sync_fd_t sfd=sync_session_fd(c->session);
            if (sfd!=SYNC_FD_INVALID) sync_shutdown_fd(sfd);
        }
        c->job_send_pending=0;
        c->build_abort=1;
        c->ui.enum_active=0;   // interrupt library scan polling
        lock_give(c);
        return LYRA_MESSAGE_OK;
    case 8:
        enum_start(c);
        return LYRA_MESSAGE_OK;
    case 9: {
        // pairing source, in order: explicit op9 fields (programmatic/test
        // path), else the user-editable host settings (config_text reads of
        // the schema parameters declared above).
        char ipb[64],portb[16],pskb[SYNC_KEY_SIZE*2+1];
        const char *ip=NULL,*port=NULL,*pskhex=NULL;
        LyraFieldRuleV3 rules[]={{1,LYRA_FIELD_UTF8},{2,LYRA_FIELD_UTF8},{3,LYRA_FIELD_UTF8}};
        int v=lyra_message_fields(msg,rules,3);
        if (v) return v;
        for (uint32_t i=0;i<msg->field_count;i++) {
            const LyraFieldV3 *f=&msg->fields[i];
            if (f->tag==1) ip=(const char*)f->data;
            else if (f->tag==2) port=(const char*)f->data;
            else if (f->tag==3) pskhex=(const char*)f->data;
        }
        if (!ip && !port && !pskhex) {
            // no explicit fields: read the user's settings form values
            if (cfg_read_pairing(c,ipb,sizeof ipb,portb,sizeof portb,
                                 pskb,sizeof pskb)) {
                toast_locked(c,"no peer settings found");
                return LYRA_MESSAGE_UNSUPPORTED;
            }
            ip=ipb; port=portb; pskhex=pskb;
        }
        if (!ip||!port||!pskhex||strlen(pskhex)<SYNC_KEY_SIZE*2)
            return LYRA_MESSAGE_INVALID;
        uint8_t psk[SYNC_KEY_SIZE];
        for (int i=0;i<SYNC_KEY_SIZE;i++) {
            unsigned x;
            if (sscanf(pskhex+i*2,"%2x",&x)!=1) return LYRA_MESSAGE_INVALID;
            psk[i]=(uint8_t)x;
        }
        lock_take(c);
        int sel=0;
        for (int i=0;i<c->track_count;i++) if (c->tracks[i].selected) sel++;
        if (!sel) {
            lock_give(c);
            toast_locked(c,"no tracks selected");
            return LYRA_MESSAGE_UNSUPPORTED;
        }
        if (c->job_send_pending || c->session) {
            lock_give(c);
            toast_locked(c,"busy");
            return LYRA_MESSAGE_UNSUPPORTED;
        }
        // snapshot the FULL selection into the batch queue. The worker sends
        // it in MAX_SYNC_PLAN_FILES-sized batches; nothing is silently
        // truncated. If a queue is already active, refuse (busy covers it,
        // but double-guard here since the busy check ran before).
        int n=0;
        for (int i=0;i<c->track_count;i++)
            if (c->tracks[i].selected) n++;
        TrackInfo *snap=(TrackInfo*)calloc((size_t)n,sizeof(TrackInfo));
        if (!snap) { lock_give(c); return LYRA_MESSAGE_UNSUPPORTED; }
        int k=0;
        for (int i=0;i<c->track_count;i++)
            if (c->tracks[i].selected) snap[k++]=c->tracks[i];
        free(c->batch_queue);
        c->batch_queue=snap;
        c->batch_total=n;
        c->batch_sent=0;
        c->batch_done=0; c->batch_skipped=0;
        // first batch slice
        int first=n<MAX_SYNC_PLAN_FILES?n:MAX_SYNC_PLAN_FILES;
        c->job_tracks=(TrackInfo*)calloc((size_t)first,sizeof(TrackInfo));
        if (!c->job_tracks) { free(c->batch_queue); c->batch_queue=NULL;
            lock_give(c); return LYRA_MESSAGE_UNSUPPORTED; }
        memcpy(c->job_tracks,c->batch_queue,(size_t)first*sizeof(TrackInfo));
        c->job_count=first;
        snprintf(c->job_ip,sizeof c->job_ip,"%s",ip);
        snprintf(c->job_port,sizeof c->job_port,"%s",port);
        memcpy(c->job_psk,psk,SYNC_KEY_SIZE);
        c->job_send_pending=1;
        lock_give(c);                       // single unlock; every early
        start_worker(c);                    // return above gives first
        return LYRA_MESSAGE_OK; }
    case 11: {   // set conflict policy for incoming same-name files
        // field 1: i64 policy (1=skip 2=overwrite 3=rename-new; 0=ask)
        LyraFieldRuleV3 rules[]={{1,LYRA_FIELD_I64}};
        int v=lyra_message_fields(msg,rules,1);
        if (v) return v;
        int64_t pol=0;
        for (uint32_t i=0;i<msg->field_count;i++)
            if (msg->fields[i].tag==1) { memcpy(&pol,msg->fields[i].data,sizeof pol); break; }
        if (pol<0 || pol>3) return LYRA_MESSAGE_INVALID;
        c->conflict_policy=(int)pol;
        return LYRA_MESSAGE_OK; }
    case 12: {  // confirm the pending first-time peer (pairing gate)
        lock_take(c);
        if (c->pair_pending[0]) {
            c->pair_pending[0]=0;           // decided: worker proceeds
        }
        lock_give(c);
        return LYRA_MESSAGE_OK; }
    case 13: {  // deny the pending peer
        lock_take(c);
        c->pair_pending[0]='n';             // non-zero => rejected
        lock_give(c);
        return LYRA_MESSAGE_OK; }
    default:
        return LYRA_MESSAGE_UNSUPPORTED;
    }
}

// ---------- default root view (frame plugin draws the whole UI) ----------
static void px_text(LyraTextLayerV3 *t, const char *s, float x, float y, float size_px) {
    memset(t,0,sizeof *t);
    t->size=sizeof *t;
    t->text=s; t->x=x; t->y=y; t->text_size=size_px;
    t->r=t->g=t->b=t->a=t->opacity=1;
}
static int on_frame(const LyraHostV3 *host, int64_t time_ns) {
    SyncCtx *c=&g_ctx;
    if (!c->initialized) return 0;
    enum_poll(c);
    // ---- default root via the service context (v0.2.4). Public contract
    // (docs/plugins/service-context): a root capability reuses the host's
    // default implementation with frame(handle, LYRA_SERVICE_DEFAULT, now,
    // box) — "插件决定是否调用、调用哪些默认组件". A submitted scene REPLACES
    // the default ("默认绘制不调用替换它的插件，也不会自动在插件结果上再叠
    // 加一层"), so the overlay is submitted ONLY while there is sync activity
    // or a prompt the user must see; otherwise the default root renders.
    // section() is re-probed EVERY frame: rotation/window size changes move
    // the region, and a cached rect would draw at a stale position.
    int busy = c->ui.screen!=UI_HOME || c->ui.enum_active ||
               c->pair_pending[0]!=0 ||
               (c->ui.toast[0] && sync_now_ms()<c->ui.toast_until_ms);
    if (!busy) {
        c->panel_visible=0;     // overlay off: default root owns ALL input
        if (c->svc && c->root_handle)
            c->svc->frame(c->svc->user,c->root_handle,LYRA_SERVICE_DEFAULT,
                          time_ns,NULL);
        return 0;
    }
    // region probe (every frame while overlay is up)
    float sx=0,sy=0,sw=0,sh=0;
    int have_region = host->section &&
        host->section(host->user,CAP_NAME,&sx,&sy,&sw,&sh)==0 && sw>0 && sh>0;
    float d=host->density(host->user);
    float W=have_region?sw:720*d, H=have_region?sh:1280*d;
    float ox=have_region?sx:0, oy=have_region?sy:0;
    c->ui.px_w=(int)W; c->ui.px_h=(int)H;
    LyraSceneV3 scene; memset(&scene,0,sizeof scene);
    scene.size=sizeof scene;
    LyraSolid bg; memset(&bg,0,sizeof bg);
    bg.x=ox; bg.y=oy; bg.w=W; bg.h=H; bg.r=0.09f; bg.g=0.09f; bg.b=0.11f; bg.a=1;
    LyraSolid bar; memset(&bar,0,sizeof bar);
    bar.x=ox; bar.y=oy; bar.w=W; bar.h=76*d; bar.r=0.14f; bar.g=0.14f; bar.b=0.18f; bar.a=1;
    LyraSolid bottom; memset(&bottom,0,sizeof bottom);
    bottom.x=ox; bottom.y=oy+H-110*d; bottom.w=W; bottom.h=110*d;
    bottom.r=0.14f; bottom.g=0.14f; bottom.b=0.18f; bottom.a=1;
    LyraSolid solids[3]={bg,bar,bottom};
    scene.solids=solids; scene.solid_count=3;
    LyraTextLayerV3 texts[10];   // title+2 status+chips+5 rows+status+toast
    // per-layer buffers: submit copies strings, but the layers themselves
    // must not alias one stack buffer (mock submits must see distinct text).
    static char line0[160], line1[160], line2[160], line3[160],
                line4[160], line5[160], line6[160], line7[160], line8[160];
    char *const line=line0;          // scratch for formatting (overwritten)
    int tn=0;
    px_text(&texts[tn++],"Lyra Player sync plugin",ox+20*d,oy+24*d,22*d);
    (void)line1;
    switch (c->ui.screen) {
    case UI_SENDING: case UI_RECEIVING: {
        lock_take(c);
        SyncProgress p=c->ui.progress;
        lock_give(c);
        snprintf(line1,sizeof line1,"%s  ok %.1f/%.1f MB  %.2f MB/s  eta %llus",
                 sync_state_name(p.task_state),
                 (double)p.confirmed_bytes/1048576.0,(double)p.total_bytes/1048576.0,
                 p.speed_bps/1048576.0,(unsigned long long)p.eta_sec);
        px_text(&texts[tn++],line1,ox+20*d,oy+100*d,15*d);
        snprintf(line2,sizeof line2,"cur: %s (%d/%d skip %d)",
                 p.current_path[0]?p.current_path:"-",p.files_done,p.file_count,
                 p.files_skipped);
        px_text(&texts[tn++],line2,ox+20*d,oy+126*d,14*d);
        break; }
    case UI_RECEIVE_WAIT:
        px_text(&texts[tn++],"waiting for peer (port 47521)...",ox+20*d,oy+100*d,16*d);
        break;
    case UI_RESULT:
        px_text(&texts[tn++],c->ui.result_summary,ox+20*d,oy+100*d,15*d);
        break;
    default: {
        // batch chips strip
        px_text(&texts[tn++],"[all] (left half)  [none] (right half)",
                ox+20*d,oy+92*d,13*d);
        int shown=0;
        char *rowbuf[LIST_ROWS]={line3,line4,line5,line6,line7};
        for (int i=c->ui.list_top;i<c->track_count && shown<LIST_ROWS;i++,shown++) {
            snprintf(rowbuf[shown],160,"%s  %s",
                     c->tracks[i].selected?"[x]":"[ ]",c->tracks[i].title);
            px_text(&texts[tn++],rowbuf[shown],ox+20*d,
                    oy+LIST_TOP_PAD*d+(float)shown*LIST_ROW_H*d,14*d);
        }
        if (!c->track_count)
            px_text(&texts[tn++],"library empty (waiting for enum)",ox+20*d,oy+100*d,15*d);
        break; }
    }
    { int sel=0;
      for (int i=0;i<c->track_count;i++) if (c->tracks[i].selected) sel++;
      snprintf(line8,sizeof line8,
               "selected %d / tracks %d - rx:%s skip:%d new:%d batch %d/%d",
               sel,c->track_count,c->receiving_enabled?"on":"off",
               c->conflicts_skipped,c->conflicts_renamed,
               c->batch_sent,c->batch_total); }
    px_text(&texts[tn++],line8,ox+20*d,oy+H-96*d,14*d);
    if (c->pair_pending[0] && tn<6)
        px_text(&texts[tn++],"pairing request - tap [OK] / [DENY] below",
                ox+20*d,oy+H-60*d,14*d);
    else if (c->ui.toast[0] && sync_now_ms()<c->ui.toast_until_ms && tn<6)
        px_text(&texts[tn++],c->ui.toast,ox+20*d,oy+H-60*d,14*d);
    scene.text_layers=texts; scene.text_layer_count=tn;
    host->submit(host->user,&scene);
    c->panel_visible=1;         // overlay is on screen: we own gestures on it
    return 0;
}
// overlay buttons: two tap targets the user can actually press. Their rects
// are derived exactly as drawn: a row above the bottom bar, split in half.
#define PAIR_BTN_H (52.f)
static int pair_btn_rect(const SyncCtx *c, int right, float *bx, float *by,
                         float *bw, float *bh) {
    const LyraHostV3 *host=c->host;
    float sx=0,sy=0,sw=0,sh=0;
    int have_region = host->section &&
        host->section(host->user,CAP_NAME,&sx,&sy,&sw,&sh)==0 && sw>0 && sh>0;
    float d=host->density(host->user);
    float W=have_region?sw:720*d, H=have_region?sh:1280*d;
    float ox=have_region?sx:0, oy=have_region?sy:0;
    *bw=W/2-16*d; *bh=PAIR_BTN_H*d;
    *bx=ox+(right? W-*bw-12*d : 12*d);
    *by=oy+H-110*d-*bh-6*d;
    return c->pair_pending[0]!=0;
}
static int point_in_panel(SyncCtx *c, const LyraHostV3 *host,
                          const LyraInput *input);
static int on_input(const LyraHostV3 *host, const LyraInput *input) {
    SyncCtx *c=&g_ctx;
    if (!c->initialized) return 0;
    if (!input || input->size!=sizeof(*input)) return 0;   // malformed event
    // ---- gesture ownership (v0.2.6) ----
    // DOWN decides the handler for the whole gesture; UP/CANCEL follow it.
    if (input->action==LYRA_INPUT_DOWN) {
        c->gesture_owner = (c->panel_visible && point_in_panel(c,host,input))
                           ? 1 : 0;
    }
    // While the panel is NOT visible the default root owns everything, even a
    // stray UP from a stale gesture.
    if (!c->panel_visible && c->gesture_owner==0) {
        if (c->svc && c->root_handle)
            return c->svc->input(c->svc->user,c->root_handle,LYRA_SERVICE_DEFAULT,input);
        return 0;
    }
    // A gesture the default root owns (DOWN was outside the panel): every
    // event of that gesture goes there untouched.
    if (c->gesture_owner==0) {
        if (c->svc && c->root_handle)
            return c->svc->input(c->svc->user,c->root_handle,LYRA_SERVICE_DEFAULT,input);
        return 0;
    }
    // From here on the panel owns this gesture (DOWN hit the visible panel).
    if (input->action==LYRA_INPUT_MOVE) return 1;      // consumed
    if (input->action==LYRA_INPUT_CANCEL) return 1;    // CANCEL ONLY ends the
        // gesture: it never activates confirm/deny/receive/toggle, even when
        // the cancelled touch sits on a control (a cancel is an abandonment,
        // not a press).
    if (input->action!=LYRA_INPUT_UP) return 1;        // DOWN: press begins,
        // fires nothing — actions fire on release only (a DOWN landing on
        // [OK] must not confirm; the UP that follows decides).
    // pairing buttons first: they float over any screen while a request pends
    // (UP only from here down)
    if (c->pair_pending[0]) {
        float bx,by,bw,bh;
        pair_btn_rect(c,0,&bx,&by,&bw,&bh);
        if (input->x>=bx && input->x<=bx+bw && input->y>=by && input->y<=by+bh) {
            LyraMessageV3 ok12; memset(&ok12,0,sizeof ok12);
            ok12.size=sizeof ok12; ok12.operation=12;
            cap_request(NULL,&ok12,NULL);
            toast(c,"peer confirmed");
            return 1;
        }
        pair_btn_rect(c,1,&bx,&by,&bw,&bh);
        if (input->x>=bx && input->x<=bx+bw && input->y>=by && input->y<=by+bh) {
            LyraMessageV3 no13; memset(&no13,0,sizeof no13);
            no13.size=sizeof no13; no13.operation=13;
            cap_request(NULL,&no13,NULL);
            toast(c,"peer denied");
            return 1;
        }
    }
    float d=host->density(host->user);
    float sx=0,sy=0,sw=0,sh=0;
    int have_region = host->section &&
        host->section(host->user,CAP_NAME,&sx,&sy,&sw,&sh)==0 && sw>0 && sh>0;
    float W=have_region?sw:720*d, H=have_region?sh:1280*d;
    float ox=have_region?sx:0, oy=have_region?sy:0;
    if (input->x<ox || input->x>ox+W || input->y<oy || input->y>oy+H) return 0;
    if (input->y > oy+H-110*d) {
        if (input->x < ox+W/2) { start_receive(c); return 1; }
        start_send(c);                   // real send: fieldless op9 (settings form)
        return 1;
    }
    if (c->ui.screen==UI_HOME && input->y>oy+90*d && input->y<oy+H-110*d) {
        // batch-select chips: "all" / "none" in the top strip of the list
        if (input->y < oy+90*d+36*d) {   // matches drawn chip strip
            if (input->x < ox+W/2) {                    // select all
                for (int i=0;i<c->track_count;i++) c->tracks[i].selected=1;
                toast(c,"selected all tracks");
            } else {                                    // clear all
                for (int i=0;i<c->track_count;i++) c->tracks[i].selected=0;
                toast(c,"selection cleared");
            }
            return 1;
        }
        // scroll: edge strips (60dp) page the list up/down
        if (input->x < ox+60*d) {
            if (c->ui.list_top>0) c->ui.list_top-=10;
            if (c->ui.list_top<0) c->ui.list_top=0;
            return 1;
        }
        if (input->x > ox+W-60*d) {
            c->ui.list_top+=10;
            if (c->ui.list_top > c->track_count-1)
                c->ui.list_top=c->track_count>0?c->track_count-1:0;
            return 1;
        }
        // row toggle
        int row=(int)((input->y-(oy+90*d+36*d))/(44*d));  // below chips
        int idx=c->ui.list_top+row;
        if (idx>=0 && idx<c->track_count) {
            c->tracks[idx].selected=!c->tracks[idx].selected;
            return 1;
        }
        return 1;
    }
    // UP/CANCEL inside our gesture but not on a control: released here,
    // gesture ends. (The default root never saw the DOWN, so it must not get
    // this UP.)
    return 1;
}
// hit-test helper for gesture ownership: same rect derivation as on_frame's
// overlay (region-aware; full screen when no section region was granted).
static int point_in_panel(SyncCtx *c, const LyraHostV3 *host,
                          const LyraInput *input) {
    float d=host->density(host->user);
    float sx=0,sy=0,sw=0,sh=0;
    int have_region = host->section &&
        host->section(host->user,CAP_NAME,&sx,&sy,&sw,&sh)==0 && sw>0 && sh>0;
    float W=have_region?sw:720*d, H=have_region?sh:1280*d;
    float ox=have_region?sx:0, oy=have_region?sy:0;
    return input->x>=ox && input->x<=ox+W && input->y>=oy && input->y<=oy+H;
}

// ---------- lifecycle ----------
static int on_init(const LyraHostV3 *host) {
    SyncCtx *c=&g_ctx;
    if (c->initialized) { c->host=host; return 0; }
    memset(c,0,sizeof *c);
    c->host=host;
    c->listen_fd=SYNC_FD_INVALID;
    c->pending_rx_fd=SYNC_FD_INVALID;
    lock_init(c);
    char base[416];
    if (sync_platform_probe(host,&c->android)==0 &&
        sync_platform_app_data_dir(&c->android,base,sizeof base)==0) {
        snprintf(c->data_dir,sizeof c->data_dir,"%s/sync",base);
    } else {
        return -1;   // no silent CWD fallback on device
    }
    sync_mkdirs(c->data_dir);
    if (sync_identity_load(c->data_dir,&c->identity)) return -1;
    batch_state_load(c);   // resume an interrupted batched send if any
    for (int i=0;i<MAX_SYNC_PLAN_FILES;i++) g_src_fd[i]=SYNC_FD_INVALID;
    g_src_count=0;
    g_commit_ctx=c;
    c->conflicts_skipped=0; c->conflicts_renamed=0;
    c->initialized=1;
    if (c->job_send_pending) {
        // process-restart auto-resume: a task persisted by a previous run
        // continues from its saved cursor (peer + credential restored from
        // the state file) as soon as the plugin loads again.
        start_worker(c);
    }
    return 0;
}
static int on_bind(const LyraHostV3 *host) {
    SyncCtx *c=&g_ctx;
    if (!c->initialized) return -1;
    c->host=host;
    return 0;
}
static void on_shutdown(const LyraHostV3 *host) {
    (void)host;
    SyncCtx *c=&g_ctx;
    if (!c->initialized) return;
    c->receiving_enabled=0;
    stop_worker(c);
    lock_take(c);
    session_cleanup_locked(c);
    lock_give(c);
    if (c->listen_fd!=SYNC_FD_INVALID) {
        sync_close(c->listen_fd);
        c->listen_fd=SYNC_FD_INVALID;
    }
    free(c->tracks); c->tracks=NULL; c->track_count=0; c->track_cap=0;
    sync_platform_shutdown(&c->android);
    c->initialized=0;
}
// Plugin-provided interfaces (docs: 参数 schema 由活动服务发布；宿主经插件
// 的 query_interface 解析 CONFIG_SCHEMA 并渲染表单)
static const void *plugin_query(const LyraHostV3 *host, uint32_t id,
                                uint32_t ver, uint32_t size) {
    (void)host;
    if (id==LYRA_INTERFACE_CONFIG_SCHEMA && ver==LYRA_INTERFACE_VERSION &&
        size==sizeof(LyraConfigSchemaV3))
        return &g_cfg_schema;
    return NULL;
}
__attribute__((visibility("default")))
int lyra_plugin_init(const LyraHostV3 *host, LyraPluginV3 *out) {
    if (!host || host->abi!=LYRA_PLUGIN_ABI || host->size!=sizeof(*host)) return -1;
    const LyraServiceContextV3 *svc = (const LyraServiceContextV3 *)
        host->query_interface(host->user,LYRA_INTERFACE_SERVICE_CONTEXT,
                              LYRA_INTERFACE_VERSION,sizeof(LyraServiceContextV3));
    if (on_init(host)) return -1;
    g_ctx.svc=svc;
    if (svc && svc->publish) {
        LyraCapabilityV3 def={sizeof(def),CAP_NAME,&g_ctx,cap_request};
        g_ctx.cap_handle=svc->publish(svc->user,&def);
    }
    // The pairing-parameter schema is served by the plugin itself through
    // LyraPluginV3.query_interface (wired below): the host resolves
    // LYRA_INTERFACE_CONFIG_SCHEMA against the plugin and renders editable
    // fields from it (public docs /plugins/config-schema).
    // resolve the default ROOT handle ("frame" capability slot). The docs'
    // root-reuse example routes through the root capability; a zero/invalid
    // handle disables default-frame/input calls (must never pass cap_handle).
    if (svc && svc->lookup) {
        uint64_t h=svc->lookup(svc->user,"frame");
        if (h) g_ctx.root_handle=h;
        else if (svc->current && !svc->lookup(svc->user,CAP_NAME))
            g_ctx.root_handle=svc->current(svc->user);   // we ARE the root
    }
    *out=(LyraPluginV3){0};
    out->size=sizeof(*out);
    out->abi=LYRA_PLUGIN_ABI;
    out->id=PLUGIN_ID;
    out->query_interface=plugin_query;
    out->config_tables=CFG_TABLE;   // consumed table for config_text reads
    out->kind="frame";
    out->version="0.3.4";
    out->init=on_init;
    out->bind=on_bind;
    out->shutdown=on_shutdown;
    out->on_frame=on_frame;
    out->on_input=on_input;
    return 0;
}
