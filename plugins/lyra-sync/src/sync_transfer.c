// sync_transfer.c - wire protocol v2 engine.
//
// Frame: tag(1) seq(4 LE) len(4 LE) nonce(12) | ct(len) | gcm_tag(16)
//   - tag+seq+len are AES-GCM AAD: header tampering breaks authentication.
//   - seq is a per-direction counter; receiver enforces exact increment
//     (replay/reorder/drop => session fails closed).
// Handshake v2: 32B random each side; receiver returns
//   SHA256("verify"||client||server) + its 16B device id, encrypted with the
//   derived key — both sides prove the PSK before any data frame.
// Resume: receiver validates temp blocks against per-block CRC32C, sends
//   HAVE_BITMAP; sender transmits only missing blocks. Auth failures and
//   protocol violations are TERMINAL (TS_FAILED); plain disconnects leave
//   the session WAITING for reconnect.
#include "sync_transfer.h"
#include "sync_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#endif

#ifdef SYNC_HOST_TEST
// Block-level receive observation for tests: invoked after each CRC-valid
// block is accepted (file_index, block, bytes). Returns nonzero to abort
// the receive loop as a plain disconnect (wire drop), letting tests cut
// the connection at an exact block boundary. Production always NULL.
int (*g_test_block_recv)(int file_index, uint32_t block, size_t bytes) = NULL;
// targeted frame injection: fires ONCE on the first received frame whose
// tag equals g_test_frame_target; production never arms it.
int g_test_frame_mode = 0;      // 1=corrupt(auth -2), 2=reorder(-3)
uint8_t g_test_frame_target = 0;// tag to intercept
int g_test_frame_once = 0;      // 1 = armed
int g_test_frame_fired = 0;     // set when the injection actually hit
int (*g_test_session_attached)(int is_sender) = NULL;
static int g_test_attach_count = 0;
// bitmap observation: fires after build_have_bitmap validates the stored
// temp (file_index, total_blocks, valid_blocks). Tests use it to assert the
// exact reuse set per session. Production never arms it.
int (*g_test_bitmap_built)(int file_index, uint32_t total, uint32_t valid) = NULL;
#endif

#define SYNC_WIRE_HDR 21          // tag(1) seq(4) len(4) nonce(12)

// ---------- session ----------
struct SyncSession {
    SyncEngine eng;
    int is_sender;
    uint8_t psk[SYNC_KEY_SIZE];
    uint8_t key_tx[SYNC_KEY_SIZE], key_rx[SYNC_KEY_SIZE];
    sync_fd_t fd;
    char data_dir[512];
    SyncManifest manifest;        // borrowed (sender) or owned (receiver)
    int manifest_owned;
    int have_manifest;
    uint64_t task_id;
    SyncTaskDb *db;

    int paused, canceled;
    uint32_t seq_tx, seq_rx;
    char peer_device[65];
    char own_device[65];
    uint8_t manifest_id[32];
    uint64_t nonce_tx, nonce_rx;
    uint8_t verify_local[32];

    int state;
    int cur_file;
    uint64_t confirmed_bytes, sent_bytes, received_bytes, total_bytes, resumed_bytes;
    uint64_t file_validated;
    uint64_t last_flush_bytes;
    uint64_t speed_window_start, speed_window_bytes;
    double speed_bps;
    uint64_t eta_sec;
    char current_path[SYNC_MAX_NAME + 1];
    char error[128];
    int files_done, files_failed, files_skipped;
    uint32_t pump_cursor;           // next manifest entry for step-wise pump
    // pending HAVE_BITMAP cache (next file's bitmap can arrive while the
    // sender is still awaiting the current file's COMMIT_OK)
    int pending_bm_valid;
    uint32_t pending_bm_file;
    uint32_t pending_bm_bytes;
    uint8_t *pending_bm;
};

void sync_session_set_own_device(SyncSession *s, const char *device_id_hex);
void sync_session_task_decl(void);

SyncSession *sync_session_create(const SyncEngine *eng, const char *data_dir,
                                 int is_sender, const uint8_t psk[SYNC_KEY_SIZE]) {
    SyncSession *s = (SyncSession *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->eng = *eng; s->is_sender = is_sender;
    memcpy(s->psk, psk, SYNC_KEY_SIZE);
    snprintf(s->data_dir, sizeof s->data_dir, "%s", data_dir);
    s->fd = SYNC_FD_INVALID;
    s->state = TS_IDLE;
    return s;
}

void sync_session_destroy(SyncSession *s) {
    if (!s) return;
    if (s->fd != SYNC_FD_INVALID) sync_close(s->fd);
    if (s->manifest_owned) manifest_free(&s->manifest);
    sync_db_close(s->db);
    free(s);
}

static void set_state(SyncSession *s, int st) { s->state = st; }

// task identity = (peer device id, manifest id)
static uint64_t task_key(SyncSession *s) {
    uint64_t h=1469598103934665603ull;
    const char *p=s->peer_device;
    while (*p) { h^=(uint8_t)*p++; h*=1099511628211ull; }
    h^=0xFF; h*=1099511628211ull;
    for (int i=0;i<32;i++) { h^=s->manifest_id[i]; h*=1099511628211ull; }
    return h;
}
static void refresh_task_id(SyncSession *s) { s->task_id=task_key(s); }

static uint32_t block_crc(const SyncFileMeta *f, uint32_t blk) {
    uint64_t nb=(f->size+SYNC_BLOCK-1)/SYNC_BLOCK;
    if (!f->block_crcs || blk>=nb) return 0;
    return f->block_crcs[blk];
}

// forward decls (handshake uses the wire)
static int wire_send(SyncSession *s, uint8_t tag, const uint8_t *payload, size_t n);
static int wire_recv(SyncSession *s, uint8_t *tag, uint32_t *blk,
                     uint8_t *payload, size_t cap, size_t *out_n);

// ---------- handshake ----------
static void make_nonce(uint8_t out[12], uint64_t ctr);
static void make_nonce(uint8_t out[12], uint64_t ctr) {
    out[0] = 0x10;
    for (int i = 1; i < 8; i++) out[i] = 0;
    for (int i = 0; i < 4; i++) out[8 + i] = (uint8_t)(ctr >> (i * 8));
}
static int handshake(SyncSession *s) {
    uint8_t mine[32], theirs[32];
    if (rand_bytes_strong(mine, sizeof mine)) {
        snprintf(s->error, sizeof s->error, "no OS entropy: refusing handshake");
        return -1;
    }
    if (sync_send_all(s->fd, mine, sizeof mine)) {
        snprintf(s->error, sizeof s->error, "handshake send failed");
        return -1;
    }
    if (sync_recv_all(s->fd, theirs, sizeof theirs) != 1) {
        snprintf(s->error, sizeof s->error, "handshake recv failed");
        return -1;
    }
    uint8_t salt[64], okm[SYNC_KEY_SIZE * 2];
    if (s->is_sender) { memcpy(salt, mine, 32); memcpy(salt + 32, theirs, 32); }
    else              { memcpy(salt, theirs, 32); memcpy(salt + 32, mine, 32); }
    hkdf_sha256(salt, sizeof salt, s->psk, SYNC_KEY_SIZE,
                "lyra-sync v2 keys", 17, okm, sizeof okm);
    if (s->is_sender) { memcpy(s->key_tx, okm, 32);     memcpy(s->key_rx, okm + 32, 32); }
    else              { memcpy(s->key_tx, okm + 32, 32); memcpy(s->key_rx, okm, 32); }
    sha256_ctx hc; sha256_init(&hc);
    sha256_update(&hc, "verify", 6);
    sha256_update(&hc, salt, 64);
    uint8_t token[32]; sha256_final(&hc, token);
    if (s->is_sender) {
        memcpy(s->verify_local, token, 32);
        s->nonce_tx = 1; s->nonce_rx = 0x4000000000000000ull;
    } else {
        s->nonce_tx = 0x4000000000000000ull; s->nonce_rx = 1;
    }
    s->seq_tx = 0; s->seq_rx = 0;   // first frame on each side carries seq=1
    {
        uint8_t vtag; uint32_t vblk; size_t vn;
        uint8_t vbuf[48];
        if (s->is_sender) {
            int vr = wire_recv(s, &vtag, &vblk, vbuf, sizeof vbuf, &vn);
            if (vr != 1) {
                snprintf(s->error, sizeof s->error, "verify frame recv r=%d (%s)", vr, s->error);
                return -1;
            }
            if (vtag != SYNC_TAG_SESSION_VERIFY || vn != 48 ||
                memcmp(vbuf, s->verify_local, 32)) {
                snprintf(s->error, sizeof s->error, "session verify failed tag=%d n=%zu", vtag, vn);
                return -1;
            }
            for (int i = 0; i < 16; i++)
                snprintf(s->peer_device + i * 2, 3, "%02x", vbuf[32 + i]);
            s->peer_device[32] = 0;
            // identity is bidirectional (v0.2.3): reply with our device id so
            // the receiver can gate commits on a known/confirmed peer. The
            // GCM tag binds this frame to the PSK, so a fake id fails auth.
            memcpy(vbuf, token, 32);
            for (int i = 0; i < 16; i++) {
                unsigned v; if (sscanf(s->own_device + i * 2, "%2x", &v) != 1) v = 0;
                vbuf[32 + i] = (uint8_t)v;
            }
            if (wire_send(s, SYNC_TAG_SESSION_VERIFY, vbuf, 48)) {
                snprintf(s->error, sizeof s->error, "peer id send failed");
                return -1;
            }
        } else {
            memcpy(vbuf, token, 32);
            for (int i = 0; i < 16; i++) {
                unsigned v; if (sscanf(s->own_device + i * 2, "%2x", &v) != 1) v = 0;
                vbuf[32 + i] = (uint8_t)v;
            }
            if (wire_send(s, SYNC_TAG_SESSION_VERIFY, vbuf, 48)) {
                snprintf(s->error, sizeof s->error, "verify send failed");
                return -1;
            }
            // identity is bidirectional: the sender replies with ITS device
            // id (same 48-byte frame layout, token field unused by us) so the
            // receiver can gate commits on a known/confirmed peer (v0.2.3).
            // GCM authenticity of this frame already binds it to the PSK.
            int vr = wire_recv(s, &vtag, &vblk, vbuf, sizeof vbuf, &vn);
            if (vr != 1 || vtag != SYNC_TAG_SESSION_VERIFY || vn != 48) {
                snprintf(s->error, sizeof s->error, "peer id recv r=%d tag=%d",
                         vr, vr == 1 ? (int)vtag : -1);
                return -1;
            }
            for (int i = 0; i < 16; i++)
                snprintf(s->peer_device + i * 2, 3, "%02x", vbuf[32 + i]);
            s->peer_device[32] = 0;
        }
    }
    return 0;
}

void sync_session_set_own_device(SyncSession *s, const char *device_id_hex) {
    snprintf(s->own_device, sizeof s->own_device, "%s", device_id_hex ? device_id_hex : "");
}

int sync_session_attach(SyncSession *s, sync_fd_t fd) {
    s->fd = fd;
    sync_nodelay(fd, 1);
    // Handshake + first manifest frame. The receiver may hold the sender at
    // its pairing-confirm gate for up to 60s before answering the manifest
    // (v0.2.6): a 30s timeout here killed every gated first transfer
    // (sender fell to TS_WAITING at ~30s while the user was still deciding).
    // 90s covers the 60s gate plus margin; the session restores its working
    // 30s timeout after the first manifest is exchanged (see pump).
    sync_settimeo(fd, 90000, 90000);
    if (handshake(s)) { set_state(s, TS_FAILED); return -1; }
#ifdef SYNC_HOST_TEST
    ++g_test_attach_count;
    if (g_test_session_attached)
        g_test_session_attached(s->is_sender);
#endif
    return 0;
}
static int wire_send_common(SyncSession *s, uint8_t tag, uint32_t blk,
                            const uint8_t *payload, size_t n) {
    uint8_t hdr[SYNC_WIRE_HDR];
    uint32_t seq = ++s->seq_tx;
    hdr[0] = tag;
    hdr[1] = (uint8_t)seq;  hdr[2] = (uint8_t)(seq >> 8);
    hdr[3] = (uint8_t)(seq >> 16); hdr[4] = (uint8_t)(seq >> 24);
    uint32_t len = (uint32_t)n;
    hdr[5] = (uint8_t)len; hdr[6] = (uint8_t)(len >> 8);
    hdr[7] = (uint8_t)(len >> 16); hdr[8] = (uint8_t)(len >> 24);
    hdr[9] = (uint8_t)blk; hdr[10] = (uint8_t)(blk >> 8);
    hdr[11] = (uint8_t)(blk >> 16); hdr[12] = (uint8_t)(blk >> 24);
    memset(hdr + 13, 0, 8);
    uint8_t nonce[12]; make_nonce(nonce, s->nonce_tx++);
    uint8_t tagbuf[SYNC_AUTH_TAG];
    uint8_t *ct = NULL;
    if (n) {
        ct = (uint8_t *)malloc(n);
        if (!ct) return -1;
        if (gcm_encrypt(s->key_tx, nonce, hdr, SYNC_WIRE_HDR, payload, n, ct, tagbuf)) {
            free(ct); return -1;
        }
    } else {
        if (gcm_encrypt(s->key_tx, nonce, hdr, SYNC_WIRE_HDR, NULL, 0, NULL, tagbuf)) return -1;
    }
    s->sent_bytes += n + SYNC_WIRE_HDR + 12 + SYNC_AUTH_TAG;
    int rc = sync_send_all(s->fd, hdr, SYNC_WIRE_HDR)
          || sync_send_all(s->fd, nonce, 12)
          || (n && sync_send_all(s->fd, ct, n))
          || sync_send_all(s->fd, tagbuf, SYNC_AUTH_TAG);
    free(ct);
    return rc ? -1 : 0;
}
static int wire_send(SyncSession *s, uint8_t tag, const uint8_t *payload, size_t n) {
    return wire_send_common(s, tag, 0, payload, n);
}
static int wire_send_block(SyncSession *s, uint8_t tag, uint32_t blk,
                           const uint8_t *payload, size_t n) {
    return wire_send_common(s, tag, blk, payload, n);
}
static int wire_recv(SyncSession *s, uint8_t *tag, uint32_t *blk,
                     uint8_t *payload, size_t cap, size_t *out_n) {
    uint8_t hdr[SYNC_WIRE_HDR];
    int r = sync_recv_all(s->fd, hdr, SYNC_WIRE_HDR);
    if (r <= 0) return r == 0 ? 0 : -1;
    uint32_t seq = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) |
                   ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
#ifdef SYNC_HOST_TEST
    // frame-level fault injection, targeted by TAG: the fault fires on the
    // first frame whose tag equals g_test_frame_target (e.g. HAVE_BITMAP or
    // COMMIT_OK), never on unrelated frames. mode: 1=AAD corruption (auth
    // failure -2), 2=seq violation (replay/reorder -3). One-shot.
    if (g_test_frame_once && hdr[0] == g_test_frame_target) {
        g_test_frame_once = 0;
        if (g_test_frame_mode == 1) {
            hdr[0] ^= 0x01;                   // AAD corruption -> GCM failure
        } else if (g_test_frame_mode == 2) {
            seq = s->seq_rx + 3;              // replay/order violation -> -3
        }
        g_test_frame_fired = 1;               // test asserts the hit
    }
#endif
    if (seq != s->seq_rx + 1) {
        snprintf(s->error, sizeof s->error, "frame seq %u expected %u",
                 seq, s->seq_rx + 1);
        return -3;
    }
    uint32_t len = (uint32_t)hdr[5] | ((uint32_t)hdr[6] << 8) |
                   ((uint32_t)hdr[7] << 16) | ((uint32_t)hdr[8] << 24);
    if (len > cap) {
        snprintf(s->error, sizeof s->error, "oversized frame %u", len);
        return -3;
    }
    uint8_t nonce[12];
    uint8_t tagbuf[SYNC_AUTH_TAG];
    if (sync_recv_all(s->fd, nonce, 12) <= 0) return -1;
    if (len && sync_recv_all(s->fd, payload, len) <= 0) return -1;
    if (sync_recv_all(s->fd, tagbuf, SYNC_AUTH_TAG) <= 0) return -1;
    if (gcm_decrypt(s->key_rx, nonce, hdr, SYNC_WIRE_HDR, payload, len,
                    payload, tagbuf)) {
        snprintf(s->error, sizeof s->error, "frame auth failed (seq %u)", seq);
        return -2;
    }
    s->seq_rx = seq;
    s->seq_rx = seq;
    *tag = hdr[0];
    *blk = (uint32_t)hdr[9] | ((uint32_t)hdr[10] << 8) |
           ((uint32_t)hdr[11] << 16) | ((uint32_t)hdr[12] << 24);
    *out_n = len;
    return 1;
}

// ---------- helpers ----------
static void note_speed(SyncSession *s, uint64_t bytes) {
    uint64_t now = sync_monotonic_ms();
    if (!s->speed_window_start) { s->speed_window_start = now; s->speed_window_bytes = 0; }
    s->speed_window_bytes += bytes;
    if (now - s->speed_window_start >= 1000) {
        double dt = (double)(now - s->speed_window_start) / 1000.0;
        s->speed_bps = (double)s->speed_window_bytes / dt;
        s->speed_window_start = now; s->speed_window_bytes = 0;
        if (s->speed_bps > 1 && s->total_bytes > s->confirmed_bytes)
            s->eta_sec = (uint64_t)((double)(s->total_bytes - s->confirmed_bytes) / s->speed_bps);
    }
}
static uint64_t block_count(uint64_t size) { return (size + SYNC_BLOCK - 1) / SYNC_BLOCK; }

static void temp_path(SyncSession *s, int i, char *out, size_t cap) {
    snprintf(out, cap, "%s/in_%llu_%d.lypart", s->data_dir,
             (unsigned long long)s->task_id, i);
}

static int db_flush_entry(SyncSession *s, const SyncFileMeta *f, int xfer_state,
                          uint64_t received, uint32_t blocks) {
    if (!s->db) return 0;
    SyncDbEntry e; memset(&e, 0, sizeof e);
    e.task_id = s->task_id; e.index = (uint32_t)s->cur_file;
    e.xfer_state = (uint8_t)xfer_state;
    e.received_bytes = received; e.have_blocks = blocks;
    snprintf(e.rel_path, sizeof e.rel_path, "%s", f->rel_path);
    memcpy(e.sha, f->sha, 32); e.size = f->size; e.kind = f->kind;
    return sync_db_put_entries(s->db, &e, 1);
}

// ---------- manifest exchange ----------
static int recv_manifest(SyncSession *s) {
    uint8_t tag; uint32_t blk; size_t n;
    size_t cap = SYNC_MAX_MANIFEST_BYTES;
    uint8_t *buf = (uint8_t *)malloc(cap);
    size_t have = 0;
    if (!buf) { snprintf(s->error, sizeof s->error, "manifest oom"); return -1; }
    for (;;) {
        int r = wire_recv(s, &tag, &blk, buf + have, cap - have, &n);
        if (r <= 0) { snprintf(s->error, sizeof s->error, "manifest hdr read r=%d tag=%d", r, (int)tag); free(buf); return -1; }
        if (tag == SYNC_TAG_REQUEST_MANIFEST) continue;
        if (tag == SYNC_TAG_PING) { wire_send(s, SYNC_TAG_PONG, NULL, 0); continue; }
        if (tag == SYNC_TAG_MANIFEST_BEGIN) { have = 0; break; }
        snprintf(s->error, sizeof s->error, "manifest unexpected tag=%d", (int)tag);
        free(buf); return -1;
    }
    for (;;) {
        int r = wire_recv(s, &tag, &blk, buf + have, cap - have, &n);
        if (r <= 0) { snprintf(s->error, sizeof s->error, "manifest chunk read r=%d tag=%d", r, (int)tag); free(buf); return -1; }
        if (tag == SYNC_TAG_MANIFEST_CHUNK) {
            have += n;
            if (have > cap) { snprintf(s->error, sizeof s->error, "manifest too large"); free(buf); return -1; }
            continue;
        }
        if (tag == SYNC_TAG_MANIFEST_END) break;
        snprintf(s->error, sizeof s->error, "manifest chunk unexpected tag=%d", (int)tag);
        free(buf); return -1;
    }
    if (manifest_unpack(buf, have, &s->manifest)) {
        snprintf(s->error, sizeof s->error, "manifest invalid");
        free(buf); return -1;
    }
    free(buf);
    s->manifest_owned = 1;
    s->have_manifest = 1;
    memcpy(s->manifest_id, s->manifest.id, 32);
    refresh_task_id(s);
    s->total_bytes = s->manifest.total_bytes;
    set_state(s, TS_PLANNING);
    return 0;
}

// ---------- receiver: bitmap resume (non-prefix scan) ----------
static int build_have_bitmap(SyncSession *s, int file_index, const SyncFileMeta *f,
                             uint8_t **bitmap_out, uint32_t *bitmap_bytes_out,
                             uint64_t *valid_bytes_out) {
    char tp[600]; temp_path(s, file_index, tp, sizeof tp);
    uint64_t sz = 0;
    sync_file_size(tp, &sz);
    uint32_t total = (uint32_t)block_count(f->size);
    uint32_t bm_bytes = (total + 7) / 8;
    uint8_t *bm = (uint8_t *)calloc(bm_bytes ? bm_bytes : 1, 1);
    if (!bm) return -1;
    uint64_t valid = 0;
    if (sz > f->size) { sync_unlink(tp); sz = 0; }
    if (sz > 0) {
        sync_fd_t fd;
        if (sync_fopen_read(tp, &fd) == 0) {
            uint8_t *buf = (uint8_t *)malloc(SYNC_BLOCK);
            if (buf) {
                for (uint32_t b = 0; b < total; b++) {
                    uint64_t off = (uint64_t)b * SYNC_BLOCK;
                    size_t take = f->size - off > SYNC_BLOCK ? SYNC_BLOCK : (size_t)(f->size - off);
                    if (off + take > sz) break;
                    int got = sync_pread(fd, buf, take, off);
                    if (got != (int)take) break;
                    if (sync_crc32c(0, buf, take) == block_crc(f, b)) {
                        bm[b >> 3] |= (uint8_t)(1u << (b & 7));
                        valid += take;
                    }
                    // bad block: bit stays 0; later blocks still checked
                }
                free(buf);
            }
            sync_fclose(fd);
        }
    }
#ifdef SYNC_HOST_TEST
    if (g_test_bitmap_built)
        g_test_bitmap_built(file_index, total, valid);
#endif
    *bitmap_out = bm; *bitmap_bytes_out = bm_bytes; *valid_bytes_out = valid;
    return 0;
}

static int recv_file_verify(SyncSession *s, int file_index,
                            const SyncFileMeta *f, const char *tp,
                            uint32_t total_blocks) {
    set_state(s, TS_VERIFYING);
    if (f->size == 0) {
        uint8_t empty_sha[32]; sha256("", 0, empty_sha);
        if (!memcmp(empty_sha, f->sha, 32)) {
            int cr = s->eng.commit_file ? s->eng.commit_file(s->eng.user,
                    &(SyncCommitInfo){s->task_id, file_index, tp, f}) : 0;
            if (cr == SYNC_COMMIT_SKIP) {
                s->files_skipped++;
                db_flush_entry(s, f, XS_CANCELED, 0, 0);
                uint32_t fi0 = (uint32_t)file_index;
                if (wire_send(s, SYNC_TAG_COMMIT_SKIP, (uint8_t *)&fi0, 4)) {
                    snprintf(s->error, sizeof s->error, "skip ack send failed");
                    set_state(s, TS_WAITING);
                    return 1;
                }
                return 0;
            }
            if (cr == 0) {
                uint32_t fi0 = (uint32_t)file_index;
                if (wire_send(s, SYNC_TAG_COMMIT_OK, (uint8_t *)&fi0, 4)) {
                    snprintf(s->error, sizeof s->error, "commit ack send failed");
                    set_state(s, TS_WAITING);
                    return 1;
                }
                db_flush_entry(s, f, XS_DONE, 0, 0);
                s->files_done++;
                return 0;
            }
            snprintf(s->error, sizeof s->error, "commit failed: %s", f->rel_path);
            set_state(s, TS_FAILED);
            return -1;
        }
        snprintf(s->error, sizeof s->error, "sha mismatch: %s", f->rel_path);
        set_state(s, TS_FAILED);
        s->files_failed++;
        return -1;
    }
    int sha_ok = 1;
    {
        sync_fd_t rfd;
        if (sync_fopen_read(tp, &rfd)) sha_ok = 0;
        else {
            sha256_ctx hc; sha256_init(&hc);
            uint8_t *vb = (uint8_t *)malloc(SYNC_CHUNK_BYTES);
            uint64_t left = f->size, off = 0;
            if (!vb) sha_ok = 0;
            while (sha_ok && left) {
                int got = sync_pread(rfd, vb, left > SYNC_CHUNK_BYTES ? SYNC_CHUNK_BYTES : (size_t)left, off);
                if (got <= 0) { sha_ok = 0; break; }
                sha256_update(&hc, vb, (size_t)got);
                off += (uint64_t)got; left -= (uint64_t)got;
            }
            free(vb);
            uint8_t digest[32];
            if (sha_ok) { sha256_final(&hc, digest); sha_ok = !memcmp(digest, f->sha, 32); }
            sync_fclose(rfd);
        }
    }
    db_flush_entry(s, f, sha_ok ? XS_VERIFYING : XS_FAILED, f->size, total_blocks);
    if (!sha_ok) {
        snprintf(s->error, sizeof s->error, "sha mismatch: %s", f->rel_path);
        set_state(s, TS_FAILED);
        s->files_failed++;
        return -1;
    }
    if (s->eng.commit_file) {
        SyncCommitInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.task_id = s->task_id;
        ci.file_index = file_index;
        ci.temp_path = tp;
        ci.meta = f;
        int cr = s->eng.commit_file(s->eng.user, &ci);
        if (cr == SYNC_COMMIT_SKIP) {
            // conflict policy chose "keep existing": the incoming content was
            // NOT published, so this is not a successful commit. Answer with
            // an explicit SKIP frame (v0.2.4) — never COMMIT_OK — and count
            // the file as skipped, not done. Session stays healthy.
            s->files_skipped++;
            db_flush_entry(s, f, XS_CANCELED, 0, 0);
            uint32_t fi = (uint32_t)file_index;
            if (wire_send(s, SYNC_TAG_COMMIT_SKIP, (uint8_t *)&fi, 4)) {
                snprintf(s->error, sizeof s->error, "skip ack send failed");
                set_state(s, TS_WAITING);
                return 1;
            }
            return 0;
        }
        if (cr) {
            snprintf(s->error, sizeof s->error, "commit failed: %s", f->rel_path);
            set_state(s, TS_FAILED);
            return -1;
        }
    }
    {
        uint32_t fi = (uint32_t)file_index;
        if (wire_send(s, SYNC_TAG_COMMIT_OK, (uint8_t *)&fi, 4)) {
            snprintf(s->error, sizeof s->error, "commit ack send failed");
            set_state(s, TS_WAITING);
            return 1;
        }
    }
    db_flush_entry(s, f, XS_DONE, f->size, total_blocks);
    s->files_done++;
    return 0;
}

static int recv_file(SyncSession *s, int file_index) {
    SyncFileMeta *f = &s->manifest.files[file_index];
    s->cur_file = file_index;
    s->file_validated = 0; s->last_flush_bytes = 0;
    set_state(s, TS_TRANSFERRING);
    snprintf(s->current_path, sizeof s->current_path, "%s", f->rel_path);

    char tp[600]; temp_path(s, file_index, tp, sizeof tp);
    uint32_t total_blocks = (uint32_t)block_count(f->size);

    uint8_t *have_bm = NULL; uint32_t bm_bytes = 0; uint64_t valid_bytes = 0;
    if (build_have_bitmap(s, file_index, f, &have_bm, &bm_bytes, &valid_bytes)) {
        snprintf(s->error, sizeof s->error, "bitmap alloc failed");
        set_state(s, TS_FAILED);
        return -1;
    }
    uint32_t have_count = 0;
    for (uint32_t b = 0; b < total_blocks; b++)
        if (have_bm[b >> 3] & (1u << (b & 7))) have_count++;
    s->resumed_bytes += valid_bytes;
    s->received_bytes += valid_bytes;
    s->file_validated = valid_bytes;
    s->confirmed_bytes += valid_bytes;

    uint32_t fi = (uint32_t)file_index;
    size_t reqlen = 8 + bm_bytes;
    uint8_t *req = (uint8_t *)malloc(reqlen ? reqlen : 1);
    if (!req) { free(have_bm); set_state(s, TS_FAILED); return -1; }
    memcpy(req, &fi, 4);
    memcpy(req + 4, &bm_bytes, 4);
    memcpy(req + 8, have_bm, bm_bytes);
    free(have_bm);
#ifdef SYNC_HOST_TEST
#endif
    int bms_rc = wire_send_block(s, SYNC_TAG_HAVE_BITMAP, (uint32_t)file_index, req, reqlen);
#ifdef SYNC_HOST_TEST
#endif
    if (bms_rc) {
        free(req);
        snprintf(s->error, sizeof s->error, "bitmap send failed");
        set_state(s, TS_WAITING);
        return 1;
    }
    free(req);

    if (total_blocks == 0)
        return recv_file_verify(s, file_index, f, tp, total_blocks);

    uint8_t *payload = (uint8_t *)malloc(SYNC_BLOCK + 4);
    if (!payload) { set_state(s, TS_FAILED); return -1; }
    sync_fd_t tfd = SYNC_FD_INVALID;
    if (sync_fopen_write(tp, &tfd)) {
        snprintf(s->error, sizeof s->error, "open temp failed");
        free(payload); set_state(s, TS_FAILED);
        return -1;
    }
    sync_ftruncate(tfd, f->size);
    sync_fsync(tfd);

    uint32_t blocks_done = have_count;
    int src_version_error = 0;
    while (blocks_done < total_blocks && !s->canceled && !s->paused) {
        uint8_t tag; uint32_t blk; size_t n;
        int r = wire_recv(s, &tag, &blk, payload, SYNC_BLOCK + 4, &n);
        if (r == 0) {
            sync_fsync(tfd); sync_fclose(tfd);
            free(payload);
            set_state(s, TS_WAITING);
            return 1;
        }
        if (r < 0) {
            snprintf(s->error, sizeof s->error, "%s",
                     r == -2 ? "auth failure" :
                     r == -3 ? s->error : "connection lost");
            sync_fsync(tfd); sync_fclose(tfd);
            free(payload);
            if (r == -2 || r == -3) {
                // auth failure / protocol violation: terminal, not reconnectable
                set_state(s, TS_FAILED);
                return -1;
            }
            set_state(s, TS_WAITING);
            return 1;
        }
        if (tag == SYNC_TAG_CANCEL) { s->canceled = 1; break; }
        if (tag == SYNC_TAG_PAUSE)  { s->paused = 1; continue; }
        if (tag == SYNC_TAG_RESUME) { s->paused = 0; continue; }
        if (tag == SYNC_TAG_PING)   { wire_send(s, SYNC_TAG_PONG, NULL, 0); continue; }
        if (tag == SYNC_TAG_ERROR)  { src_version_error = 1; break; }
        if (tag != SYNC_TAG_BLOCK) continue;
        if (blk >= total_blocks || n < 4) continue;
        uint32_t fidx; memcpy(&fidx, payload, 4);
        if (fidx != (uint32_t)file_index) continue;
        uint8_t *data = payload + 4;
        size_t dlen = n - 4;
        uint64_t off = (uint64_t)blk * SYNC_BLOCK;
        size_t expect = (size_t)(f->size - off > SYNC_BLOCK ? SYNC_BLOCK : f->size - off);
        if (dlen != expect) continue;
#ifdef SYNC_WIRE_LOG
#endif
        if (sync_crc32c(0, data, dlen) != block_crc(f, blk)) continue;  // corrupt: drop
        if (sync_pwrite(tfd, data, dlen, off)) {
            snprintf(s->error, sizeof s->error, "write failed");
            sync_fclose(tfd); free(payload);
            set_state(s, TS_FAILED);
            return -1;
        }
        blocks_done++;
#ifdef SYNC_HOST_TEST
        // post-store hook: fires after the block is actually persisted,
        // so "hook recorded" == "block durably held by the receiver".
        // Nonzero return aborts as a plain wire drop (TS_WAITING) at an
        // exact block boundary.
        if (g_test_block_recv &&
            g_test_block_recv(file_index, blk, dlen)) {
            sync_fsync(tfd); sync_fclose(tfd);
            free(payload);
            set_state(s, TS_WAITING);
            return 1;
        }
#endif
        s->file_validated += dlen;
        s->received_bytes += dlen;
        s->confirmed_bytes += dlen;
        note_speed(s, dlen);
        uint64_t since = s->file_validated - s->last_flush_bytes;
        if (since >= SYNC_PROGRESS_INTERVAL || blocks_done == total_blocks) {
            sync_fsync(tfd);
            s->last_flush_bytes = s->file_validated;
            db_flush_entry(s, f, XS_ACTIVE, s->file_validated, blocks_done);
        }
    }
    sync_fsync(tfd); sync_fclose(tfd);
    free(payload);
    if (src_version_error) {
        snprintf(s->error, sizeof s->error, "sender aborted: source changed");
        set_state(s, TS_FAILED);
        return -1;
    }
    if (s->canceled) { set_state(s, TS_CANCELED); return 2; }
    if (s->paused)   { set_state(s, TS_WAITING);  return 1; }
    return recv_file_verify(s, file_index, f, tp, total_blocks);
}

void sync_receiver_start(SyncSession *s) {
    set_state(s, TS_PLANNING);
    s->db = sync_db_open(s->data_dir);
}

// ---------- sender ----------
int sync_sender_start(SyncSession *s, const SyncManifest *m) {
    if (!m) return -1;
    s->manifest = *m;
    s->manifest_owned = 0;
    s->have_manifest = 0;
    memcpy(s->manifest_id, m->id, 32);
    refresh_task_id(s);
    s->total_bytes = s->manifest.total_bytes;
    set_state(s, TS_PLANNING);
    s->db = sync_db_open(s->data_dir);
    return 0;
}

// Resume variant: same manifest identity, but the transfer cursor starts at
// manifest entry `first_file`. Because the manifest (and therefore its id)
// is IDENTICAL to the interrupted session, the receiver's temp file paths
// (in_<task_id>_<i>.lypart) match and previously transferred blocks are
// reused. A plain restart with a re-built manifest would produce a new
// manifest id and orphan every existing temp block — the exact failure this
// variant exists to prevent.
int sync_sender_start_at(SyncSession *s, const SyncManifest *m,
                         uint32_t first_file) {
    if (!m || first_file > m->count) return -1;
    int rc = sync_sender_start(s, m);
    if (rc) return rc;
    s->pump_cursor = first_file;
    return 0;
}

static int await_commit_ok(SyncSession *s, uint32_t fi) {
    for (;;) {
        uint8_t tag; uint32_t blk; size_t n;
        uint8_t pl[8];
        int r = wire_recv(s, &tag, &blk, pl, sizeof pl, &n);
        if (r == -2 || r == -3) {
            // auth failure / replay-order violation: TERMINAL, never
            // demoted to a plain disconnect
            if (!s->error[0])
                snprintf(s->error, sizeof s->error, "%s",
                         r == -2 ? "frame auth failed (await ack)" :
                                   "frame seq violation (await ack)");
            set_state(s, TS_FAILED);
            return 1;
        }
        if (r <= 0) { set_state(s, TS_WAITING); return 1; }
        if (tag == SYNC_TAG_CANCEL) { s->canceled = 1; set_state(s, TS_CANCELED); return 2; }
        if (tag == SYNC_TAG_PAUSE)  { s->paused = 1; continue; }
        if (tag == SYNC_TAG_RESUME) { s->paused = 0; continue; }
        if (tag == SYNC_TAG_PING)   { wire_send(s, SYNC_TAG_PONG, NULL, 0); continue; }
        // HAVE_BITMAP for the NEXT file can arrive while we are still
        // waiting for this file's ack (the receiver runs one file ahead on
        // fully-valid resumes). Buffer it whole for send_file_blocks.
#ifdef SYNC_HOST_TEST
#endif
        if (tag == SYNC_TAG_HAVE_BITMAP && blk != fi) {
            uint8_t *nb = (uint8_t*)realloc(s->pending_bm, n);
            if (nb) {
                memcpy(nb, pl, n);
                s->pending_bm = nb;
                s->pending_bm_bytes = (uint32_t)n;
                s->pending_bm_file = blk;
                s->pending_bm_valid = 1;
            }
            continue;
        }
        if (tag == SYNC_TAG_COMMIT_OK && n == 4) {
            uint32_t ack; memcpy(&ack, pl, 4);
            if (ack == fi) { s->files_done++; return 0; }
        }
        if (tag == SYNC_TAG_COMMIT_SKIP && n == 4) {
            // receiver kept its existing file (conflict policy): explicitly
            // NOT a successful commit. Count skipped and move on.
            uint32_t ack; memcpy(&ack, pl, 4);
            if (ack == fi) { s->files_skipped++; return 0; }
        }
    }
}

static int send_file_blocks(SyncSession *s, int file_index, uint8_t *payload) {
    SyncFileMeta *f = &s->manifest.files[file_index];
    s->cur_file = file_index;
    snprintf(s->current_path, sizeof s->current_path, "%s", f->rel_path);
    uint32_t total = (uint32_t)block_count(f->size);
    uint8_t *want = NULL;
    // A HAVE_BITMAP for THIS file may already be buffered (it arrived while
    // the previous file's COMMIT_OK was awaited). Consume it now.
    uint8_t from_cache = 0;
#ifdef SYNC_HOST_TEST
#endif
    if (s->pending_bm_valid && s->pending_bm_file == (uint32_t)file_index) {
        uint32_t fidx, bmb;
        memcpy(&fidx, s->pending_bm, 4); memcpy(&bmb, s->pending_bm + 4, 4);
        if (fidx == (uint32_t)file_index && bmb == (total + 7) / 8) {
            want = (uint8_t *)malloc(bmb ? bmb : 1);
            if (!want) return -1;
            memcpy(want, s->pending_bm + 8, bmb);
            for (uint32_t b = 0; b < total; b++)
                if (want[b >> 3] & (1u << (b & 7))) {
                    uint64_t off = (uint64_t)b * SYNC_BLOCK;
                    s->resumed_bytes += (f->size - off > SYNC_BLOCK) ? SYNC_BLOCK : f->size - off;
                }
            from_cache = 1;
        }
        s->pending_bm_valid = 0;
    }
    for (;;) {
        if (from_cache) { from_cache = 0; break; }
        uint8_t tag; uint32_t blk; size_t n;
        size_t bcap = 8 + (total + 7) / 8;
        uint8_t *big = (uint8_t *)malloc(bcap ? bcap : 1);
        if (!big) return -1;
        int r = wire_recv(s, &tag, &blk, big, bcap, &n);
        if (r == -2 || r == -3) {
            // auth failure / replay-order violation: propagate TERMINAL
            // (pump maps -2/-3 to TS_FAILED); never a plain disconnect
            if (!s->error[0])
                snprintf(s->error, sizeof s->error, "%s",
                         r == -2 ? "frame auth failed (bitmap wait)" :
                                   "frame seq violation (bitmap wait)");
            free(big);
            return -2;
        }
        if (r <= 0) { free(big); return -1; }
        if (tag == SYNC_TAG_HAVE_BITMAP && n >= 8) {
            uint32_t fidx, bmb;
            memcpy(&fidx, big, 4); memcpy(&bmb, big + 4, 4);
            if (fidx == (uint32_t)file_index && bmb == (total + 7) / 8) {
                want = (uint8_t *)malloc(bmb ? bmb : 1);
                if (!want) { free(big); return -1; }
                memcpy(want, big + 8, bmb);
                for (uint32_t b = 0; b < total; b++)
                    if (want[b >> 3] & (1u << (b & 7))) {
                        uint64_t off = (uint64_t)b * SYNC_BLOCK;
                        s->resumed_bytes += (f->size - off > SYNC_BLOCK) ? SYNC_BLOCK : f->size - off;
                    }
            }
            free(big);
            // first HAVE_BITMAP received: the receiver's gate (if any) is
            // passed — restore the working 30s timeouts for the block stream
            // and per-file commit acks (stalls must still fail fast).
            sync_settimeo(s->fd, 30000, 30000);
            break;
        }
        if (tag == SYNC_TAG_CANCEL) { s->canceled = 1; free(big); return 2; }
        if (tag == SYNC_TAG_PAUSE)  { s->paused = 1; free(big); continue; }
        if (tag == SYNC_TAG_RESUME) { s->paused = 0; free(big); continue; }
        if (tag == SYNC_TAG_PING)   { wire_send(s, SYNC_TAG_PONG, NULL, 0); free(big); continue; }
        free(big);
        snprintf(s->error, sizeof s->error, "expected bitmap, got tag=%d", (int)tag);
        return -1;
    }
    int result = 0;
    for (uint32_t b = 0; b < total && !s->canceled && !s->paused; b++) {
        if (want && (want[b >> 3] & (1u << (b & 7)))) continue;  // receiver holds it
        uint64_t off = (uint64_t)b * SYNC_BLOCK;
        size_t take = (size_t)(f->size - off > SYNC_BLOCK ? SYNC_BLOCK : f->size - off);
        int got = s->eng.read_source(s->eng.user, file_index, off, payload, take);
        if (got != (int)take) {
            snprintf(s->error, sizeof s->error, "source changed/short read: %s", f->rel_path);
            wire_send(s, SYNC_TAG_ERROR, NULL, 0);
            result = -2;
            break;
        }
        uint8_t framed[SYNC_BLOCK + 4];
        uint32_t fidx = (uint32_t)file_index;
        memcpy(framed, &fidx, 4);
        memcpy(framed + 4, payload, take);
        if (wire_send_block(s, SYNC_TAG_BLOCK, b, framed, take + 4)) { result = -1; break; }
        s->received_bytes += take;
        note_speed(s, take);
    }
    free(want);
    return result;
}

int sync_session_pump(SyncSession *s, int budget_ms) {
    (void)budget_ms;
    if (s->state == TS_DONE || s->state == TS_FAILED || s->state == TS_CANCELED) return 1;
    if (s->canceled) { set_state(s, TS_CANCELED); return 1; }
    if (!s->have_manifest) {
        if (s->is_sender) {
            uint8_t *packed = NULL; size_t pn = 0;
            if (manifest_pack(&s->manifest, &packed, &pn)) {
                snprintf(s->error, sizeof s->error, "manifest pack failed");
                return -1;
            }
            if (wire_send(s, SYNC_TAG_MANIFEST_BEGIN, NULL, 0) ||
                wire_send(s, SYNC_TAG_MANIFEST_CHUNK, packed, pn) ||
                wire_send(s, SYNC_TAG_MANIFEST_END, NULL, 0)) {
                free(packed);
                return -1;
            }
            free(packed);
            s->have_manifest = 1;   // manifest sent once; step-wise pump continues
            // NOTE: the working 30s timeouts are restored only after the FIRST
            // bitmap arrives (send_file_blocks) — the receiver's HAVE_BITMAP
            // answer itself may sit behind a pairing-confirm gate for up to
            // 60s, so a restore here would kill the gated first transfer.
        } else {
            if (recv_manifest(s)) return -1;
            // manifest received: gate is behind us; working timeouts onward
            sync_settimeo(s->fd, 30000, 30000);
        }
    }
    if (s->is_sender) {
        set_state(s, TS_TRANSFERRING);
        uint8_t *payload = (uint8_t *)malloc(SYNC_BLOCK);
        if (!payload) return -1;
        // step-wise: ONE manifest entry per pump call (returns 0 = more
        // work pending). The caller regains control between files so it can
        // persist batch progress for crash resume; the loop-until-nonzero
        // contract is unchanged for callers that do not need it.
        uint32_t i = s->pump_cursor;
        int rc = 0;
#ifdef SYNC_HOST_TEST
#endif
        if (!s->canceled) {
            int r = send_file_blocks(s, (int)i, payload);
            if (r == -2)      { set_state(s, TS_FAILED); rc = -3; }   // auth/protocol: terminal
            else if (r == 2)  { set_state(s, TS_CANCELED); rc = 1; }
            else if (r != 0)  { set_state(s, TS_WAITING); rc = 1; }   // plain disconnect: recoverable
            else {
#ifdef SYNC_HOST_TEST
#endif
                int a = await_commit_ok(s, i);
#ifdef SYNC_HOST_TEST
#endif
                if (a != 0)   { rc = 1; }
                else {
                    s->pump_cursor++;
                    if (s->pump_cursor >= s->manifest.count) {
                        set_state(s, TS_DONE);
                        rc = 1;
                    }
                }
            }
        }
        free(payload);
        if (rc) return rc;
        if (s->canceled) { set_state(s, TS_CANCELED); return 1; }
        if (s->paused)   { set_state(s, TS_WAITING);  return 1; }
        return 0;                    // more files pending
    } else {
        if (s->pump_cursor >= s->manifest.count) {
            set_state(s, TS_DONE);
            return 1;
        }
        int r = recv_file(s, (int)s->pump_cursor);
        if (r != 0) return 1;
        s->pump_cursor++;
        if (s->pump_cursor >= s->manifest.count) {
            set_state(s, TS_DONE);   // original loop set DONE after the last file
            return 1;
        }
        return 0;
    }
}

void sync_session_pause(SyncSession *s)  { s->paused = 1; }
void sync_session_resume(SyncSession *s) { s->paused = 0; }
void sync_session_cancel(SyncSession *s) { s->canceled = 1; }

void sync_session_get_progress(SyncSession *s, SyncProgress *out) {
    memset(out, 0, sizeof *out);
    out->task_state = s->state;
    out->file_index = s->cur_file;
    snprintf(out->current_path, sizeof out->current_path, "%s", s->current_path);
    out->confirmed_bytes = s->confirmed_bytes;
    out->sent_bytes = s->sent_bytes;
    out->received_bytes = s->received_bytes;
    out->total_bytes = s->total_bytes;
    out->speed_bps = s->speed_bps;
    out->eta_sec = s->eta_sec;
    out->verifying = (s->state == TS_VERIFYING);
    out->retrying = (s->state == TS_WAITING);
    snprintf(out->error, sizeof out->error, "%s", s->error);
    out->file_count = (int)s->manifest.count;
    out->files_done = s->files_done;
    out->files_failed = s->files_failed;
    out->files_skipped = s->files_skipped;
    out->resumed_bytes = s->resumed_bytes;
}

const char *sync_state_name(int ts) {
    switch (ts) {
        case TS_IDLE: return "idle";
        case TS_PLANNING: return "planning";
        case TS_TRANSFERRING: return "transferring";
        case TS_WAITING: return "waiting-reconnect";
        case TS_VERIFYING: return "verifying";
        case TS_DONE: return "done";
        case TS_FAILED: return "failed";
        case TS_CANCELED: return "canceled";
        case TS_ENOSPC: return "no-space";
        default: return "?";
    }
}

int sync_session_state(SyncSession *s) { return s->state; }
const char *sync_session_peer_device(SyncSession *s) { return s->peer_device; }
int sync_session_files_done(SyncSession *s) { return s->files_done; }
int sync_session_files_failed(SyncSession *s) { return s->files_failed; }
int sync_session_files_skipped(SyncSession *s) { return s->files_skipped; }
int sync_session_have_manifest(SyncSession *s) { return s->have_manifest; }
unsigned sync_session_manifest_count(SyncSession *s) { return s->manifest.count; }
sync_fd_t sync_session_fd(SyncSession *s) { return s->fd; }
uint64_t sync_session_bytes_on_wire(SyncSession *s) { return s->sent_bytes; }
uint64_t sync_session_task_id(SyncSession *s) { return s->task_id; }
