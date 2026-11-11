// sync_transfer.h - encrypted block-transfer engine v2 (sender + receiver).
//
// Protocol v2 highlights:
//  - frame header (tag/seq/len) is covered by AES-GCM AAD: header tampering
//    breaks the tag; per-direction sequence numbers detect replay/reorder.
//  - manifest v2 carries per-block CRC32C so the receiver can resume exactly:
//    it validates stored temp blocks against the CRC list and requests only
//    the missing/corrupt blocks via a HAVE_BITMAP frame.
//  - per-file commit handshake: receiver verifies SHA-256, commits into the
//    library via the platform callback, then replies COMMIT_OK; the sender
//    marks a file successful only after COMMIT_OK.
#pragma once
#include "sync_common.h"
#include "sync_io.h"
#include "sync_db.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SyncProgress {
    int task_state;            // TS_*
    int file_index;
    char current_path[SYNC_MAX_NAME + 1];
    uint64_t confirmed_bytes;  // receiver-ACKnowledged (COMMIT_OK) bytes
    uint64_t sent_bytes;       // sender: ciphertext bytes handed to the socket
    uint64_t received_bytes;   // receiver: validated bytes written
    uint64_t total_bytes;
    double speed_bps;
    uint64_t eta_sec;
    int retrying;
    int verifying;
    char error[128];
    int file_count, files_done, files_failed;
    uint64_t resumed_bytes;    // bytes reused from previous temp (not resent)
    int files_skipped;         // receiver kept its existing file (v0.2.4)
} SyncProgress;

// Everything the platform needs to commit a verified file into the library.
// temp_path is the engine-managed sparse file that hashed clean; meta carries
// rel_path/size/kind/sha. The engine never guesses destination names.
typedef struct SyncCommitInfo {
    uint64_t task_id;
    int file_index;
    const char *temp_path;      // engine-owned, valid during the call
    const SyncFileMeta *meta;   // points into the session manifest
} SyncCommitInfo;

typedef struct SyncEngine {
    // sender: read one block of a local source file (returns bytes read)
    int (*read_source)(void *user, int file_index, uint64_t off, void *buf, size_t cap);
    // receiver: persist one block to temp file (returns 0 ok)
    int (*write_dest)(void *user, int file_index, uint64_t off, const void *buf, size_t n);
    // receiver: commit one SHA-verified file.
    // Returns 0 = published; SYNC_COMMIT_SKIP = incoming content intentionally
    // not published (conflict policy) — engine must NOT treat it as committed;
    // nonzero other = hard failure.
    int (*commit_file)(void *user, const SyncCommitInfo *info);
    void *user;
} SyncEngine;

typedef struct SyncManifest {
    SyncFileMeta *files;
    uint32_t count;
    uint64_t total_bytes;
    uint8_t id[32]; // sha256 over serialized manifest
} SyncManifest;

size_t manifest_meta_size(const SyncFileMeta *f);
size_t manifest_packed_size(const SyncManifest *m);
int manifest_pack(const SyncManifest *m, uint8_t **out, size_t *out_n);
int manifest_unpack(const uint8_t *data, size_t n, SyncManifest *m);
void manifest_free(SyncManifest *m);
int manifest_compute_crcs(SyncManifest *m, const SyncEngine *eng);

typedef struct SyncSession SyncSession;

SyncSession *sync_session_create(const SyncEngine *engine, const char *data_dir,
                                 int is_sender, const uint8_t psk[SYNC_KEY_SIZE]);
void sync_session_destroy(SyncSession *);
void sync_session_set_own_device(SyncSession *, const char *device_id_hex);
int sync_session_attach(SyncSession *, sync_fd_t fd);
int sync_sender_start(SyncSession *, const SyncManifest *m);
// Resume variant: reuse the SAME manifest (same id) but start the transfer
// cursor at manifest entry first_file. Keeps receiver temp paths stable
// across reconnects so previously transferred blocks are reused.
int sync_sender_start_at(SyncSession *, const SyncManifest *m, uint32_t first_file);
void sync_receiver_start(SyncSession *);
// Step-wise progress: ONE manifest entry per call. Returns 0 = entry done,
// more pending (caller can persist batch progress between entries);
// nonzero = session reached a terminal/waiting state (check task_state).
// Callers that only need completion keep the loop-until-nonzero pattern.
int sync_session_pump(SyncSession *, int budget_ms);
void sync_session_pause(SyncSession *);
void sync_session_resume(SyncSession *);
void sync_session_cancel(SyncSession *);
void sync_session_get_progress(SyncSession *, SyncProgress *out);
const char *sync_state_name(int ts);

#define SYNC_COMMIT_SKIP 2   // commit_file: skip (conflict policy), not an error

#ifdef SYNC_HOST_TEST
// fault-injection hooks (tests only; production never defines them)
extern int (*g_test_block_recv)(int file_index, uint32_t block, size_t bytes);
// targeted frame injection: fires ONCE on the first received frame whose
// tag equals g_test_frame_target (e.g. SYNC_TAG_HAVE_BITMAP/COMMIT_OK)
extern int g_test_frame_mode;    // 1=corrupt(auth fail -2), 2=reorder(-3)
extern uint8_t g_test_frame_target;
extern int g_test_frame_once;    // 1 = armed
extern int g_test_frame_fired;   // 1 = injection hit (test asserts this)
// bitmap observation: fires after the receiver validated the stored temp
extern int (*g_test_bitmap_built)(int file_index, uint32_t total, uint32_t valid);
extern int (*g_test_session_attached)(int is_sender);
// attach observation: fires after each successful handshake; the returned
// session number is process-local and increments on every attach
extern int (*g_test_session_attached)(int is_sender);
#endif

// test/inspection accessors
int sync_session_state(SyncSession *);
const char *sync_session_peer_device(SyncSession *);   // 32-hex id, after handshake
int sync_session_files_done(SyncSession *);
int sync_session_files_failed(SyncSession *);
int sync_session_files_skipped(SyncSession *);
int sync_session_have_manifest(SyncSession *);
unsigned sync_session_manifest_count(SyncSession *);
sync_fd_t sync_session_fd(SyncSession *);
uint64_t sync_session_bytes_on_wire(SyncSession *); // actual network payload bytes
uint64_t sync_session_task_id(SyncSession *);       // (peer,manifest) task key

#ifdef __cplusplus
}
#endif
