// sync_common.h - shared protocol layout, hashing, task model.
// Lyra Player 同步插件 (chun.lyra.sync) - author chun.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_MAGIC "LYSY"
#define SYNC_PROTO_VERSION 2
// TLS-PSK: one round-trip of random material per side, HKDF-SHA256 expansion.
#define SYNC_PSK_SEED_BYTES 32
#define SYNC_KEY_SIZE 32
#define SYNC_SALT_SIZE 16
#define SYNC_HANDSHAKE_BYTES 96
#define SYNC_AUTH_TAG 16
#define SYNC_NONCE_SIZE 12
#define SYNC_BLOCK 262144          // 256 KiB payload blocks on the wire
#define SYNC_CHUNK_BYTES (4u<<20)  // 4 MiB sparse-file map granularity
#define SYNC_MAX_NAME 400          // bytes of one UTF-8 path component
#define SYNC_MAX_FILES 100000
#define SYNC_MAX_MANIFEST_BYTES (64u<<20)
#define SYNC_MAX_MANIFEST_ENTRIES 20000
#define SYNC_PROGRESS_INTERVAL (4u<<20)  // fdatasync+DB flush cadence
#define SYNC_SCAN_PORT 47521
#define SYNC_MAX_BATCH_DB 512
#define SYNC_MAX_FILE_BLOCKS 131072  // 256KiB*131072 = 32 GiB max/file
#define SYNC_MANIFEST_CRC_LIMIT 0    // 0 = no per-file block-count cap beyond the above

typedef struct TrackInfo {
    char uid[64];
    char title[96];
    char fname[128];
    int64_t track_id;
    uint64_t size;
    int selected;
} TrackInfo;

typedef struct SyncFileMeta {
    char rel_path[SYNC_MAX_NAME + 1];
    char playlist_ref[SYNC_MAX_NAME + 1]; // playlist row only: source-relative target
    uint64_t size;
    uint64_t mtime_ns;
    uint8_t sha[32];
    uint8_t kind; // 0 audio, 1 cover, 2 lyrics, 3 cue
    uint8_t in_playlist;
    uint32_t *block_crcs; // CRC32C of each 256KiB block (sender computes; heap)
} SyncFileMeta;

// xfer_state: 0 pending, 1 active, 2 verifying, 3 done, 4 failed, 5 canceled
enum { XS_PENDING=0, XS_ACTIVE=1, XS_VERIFYING=2, XS_DONE=3, XS_FAILED=4, XS_CANCELED=5 };
enum { FK_AUDIO=0, FK_COVER=1, FK_LYRICS=2, FK_CUE=3 };

// task_state: 0 idle, 1 planning, 2 transferring, 3 waiting-reconnect,
// 4 final verify, 5 done, 6 failed, 7 canceled, 8 no-space
enum { TS_IDLE=0, TS_PLANNING=1, TS_TRANSFERRING=2, TS_WAITING=3,
       TS_VERIFYING=4, TS_DONE=5, TS_FAILED=6, TS_CANCELED=7, TS_ENOSPC=8 };

static inline uint32_t sync_crc32c(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78u & (-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// Wire layout: block header + payload. tag(1) block_index(4 BE) len(4 BE)
// nonce(12) payload(len) tag(16). Header CRC32C(16 bytes) appended so the
// receiver can resync after garbage instead of desynchronizing forever.
#define SYNC_HDR 9
#define SYNC_WIRE_HDR 21   // v2: tag(1) seq(4) len(4) nonce(12)
static const uint8_t SYNC_TAG_HELLO=1, SYNC_TAG_BYE=2, SYNC_TAG_PING=3,
    SYNC_TAG_PONG=4, SYNC_TAG_REQUEST_MANIFEST=5, SYNC_TAG_MANIFEST_BEGIN=6,
    SYNC_TAG_MANIFEST_CHUNK=7, SYNC_TAG_MANIFEST_END=8, SYNC_TAG_REQUEST_BLOCKS=9,
    SYNC_TAG_BLOCK=10, SYNC_TAG_ACK=11, SYNC_TAG_COMMIT=12, SYNC_TAG_COMMIT_OK=13,
    SYNC_TAG_ERROR=14, SYNC_TAG_PAUSE=15, SYNC_TAG_RESUME=16, SYNC_TAG_CANCEL=17,
    SYNC_TAG_HAVE_BITMAP=18, SYNC_TAG_SESSION_VERIFY=19,
    // v0.2.4: incoming content NOT published (conflict policy kept the
    // existing file). Distinct from COMMIT_OK so the sender never counts a
    // skipped file as committed. Payload: u32 file_index.
    SYNC_TAG_COMMIT_SKIP=20;

#ifdef __cplusplus
}
#endif
