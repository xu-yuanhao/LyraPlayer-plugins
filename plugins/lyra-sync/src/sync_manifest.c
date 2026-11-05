// sync_transfer.c - wire protocol v2: CRC chunk manifest, bitmap resume,
// authenticated headers, sequence numbers, receiver commit confirmation.
//
// Durability contract on receive:
//   1. block decrypted+tag-ok (header covered by AAD) -> pwrite to temp
//   2. per-file every SYNC_PROGRESS_INTERVAL validated bytes: fdatasync temp,
//      then DB update AFTER fsync (never mark done before data is durable)
//   3. all blocks: SHA-256 over temp must equal manifest sha
//   4. SHA ok -> platform commit_file (rename into library) -> COMWAIT_OK sent
//      after commit callback returns 0; only then sender counts the file done.
// Resume: receiver computes its have-bitmap from temp size + per-block CRC
// (blake-free: CRC32C of each stored 256KiB block vs manifest chunk CRCs) and
// requests exactly the missing/corrupt blocks via REQUEST_BLOCKS bitmap.
#include "sync_transfer.h"
#include "sync_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#endif

// ---------- manifest serialization v2 ----------
// header: u32 version=2 | u32 count | u64 total_bytes
// per file: kind(1) in_playlist(1) rsv(2) u64 size u64 mtime_ns u32 block_count
//           sha(32) | u16 rel_len | rel_path | u16 pl_len | playlist_ref
//           | u32 crc_per_block * block_count (CRC32C of each 256KiB block)
static void put_u64(uint8_t **p, uint64_t v){ for(int i=0;i<8;i++) *(*p)++=(uint8_t)(v>>(i*8)); }
static uint64_t get_u64(const uint8_t **p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)(*(*p)++)<<(i*8); return v; }
static void put_u32(uint8_t **p, uint32_t v){ for(int i=0;i<4;i++) *(*p)++=(uint8_t)(v>>(i*8)); }
static uint32_t get_u32(const uint8_t **p){ uint32_t v=0; for(int i=0;i<4;i++) v|=(uint32_t)(*(*p)++)<<(i*8); return v; }
static void put_u16(uint8_t **p, uint16_t v){ *(*p)++=(uint8_t)v; *(*p)++=(uint8_t)(v>>8); }
static uint16_t get_u16(const uint8_t **p){ uint16_t v=(uint16_t)(*(*p)++); v|=(uint16_t)((uint16_t)(*(*p)++)<<8); return v; }

static uint32_t block_crc(const SyncFileMeta *f, uint32_t blk) {
    uint64_t nb = (f->size + SYNC_BLOCK - 1) / SYNC_BLOCK;
    if (!f->block_crcs || blk >= nb) return 0;
    return f->block_crcs[blk];
}

size_t manifest_meta_size(const SyncFileMeta *f) {
    uint64_t nb = (f->size + SYNC_BLOCK - 1) / SYNC_BLOCK;
    return 4 + 8 + 8 + 4 + 32 + 2 + strlen(f->rel_path) + 2 + strlen(f->playlist_ref)
         + (size_t)nb * 4;
}
size_t manifest_packed_size(const SyncManifest *m) {
    size_t total = 4 + 4 + 8;
    for (uint32_t i = 0; i < m->count; i++) total += manifest_meta_size(&m->files[i]);
    return total;
}
int manifest_pack(const SyncManifest *m, uint8_t **out, size_t *out_n) {
    if (!m || !out || !out_n) return -1;
    if (m->count > SYNC_MAX_MANIFEST_ENTRIES) return -1;
    size_t total = manifest_packed_size(m);
    if (total > SYNC_MAX_MANIFEST_BYTES) return -1;
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return -1;
    uint8_t *p = buf;
    put_u32(&p, 2);                 // version
    put_u32(&p, m->count);
    put_u64(&p, m->total_bytes);
    for (uint32_t i = 0; i < m->count; i++) {
        const SyncFileMeta *f = &m->files[i];
        size_t rl = strlen(f->rel_path), pl = strlen(f->playlist_ref);
        if (rl > SYNC_MAX_NAME || pl > SYNC_MAX_NAME) { free(buf); return -1; }
        uint64_t nb = (f->size + SYNC_BLOCK - 1) / SYNC_BLOCK;
        if (nb > 0xFFFFFFFFull) { free(buf); return -1; }
        *p++ = f->kind; *p++ = f->in_playlist; *p++ = 0; *p++ = 0;
        put_u64(&p, f->size); put_u64(&p, f->mtime_ns);
        put_u32(&p, (uint32_t)nb);
        memcpy(p, f->sha, 32); p += 32;
        put_u16(&p, (uint16_t)rl); memcpy(p, f->rel_path, rl); p += rl;
        put_u16(&p, (uint16_t)pl); memcpy(p, f->playlist_ref, pl); p += pl;
        for (uint64_t b = 0; b < nb; b++) put_u32(&p, block_crc(f, (uint32_t)b));
    }
    if ((size_t)(p - buf) != total) { free(buf); return -1; }  // sizing invariant
    sha256(buf, total, (uint8_t*)m->id);
    *out = buf; *out_n = total;
    return 0;
}
int manifest_unpack(const uint8_t *data, size_t n, SyncManifest *m) {
    memset(m, 0, sizeof *m);
    const size_t HDR = 4 + 4 + 8;
    if (!data || n < HDR || n > SYNC_MAX_MANIFEST_BYTES) return -1;
    const uint8_t *p = data;
    uint32_t version = get_u32(&p);
    if (version != 2) return -1;
    uint32_t count = get_u32(&p);
    if (count > SYNC_MAX_MANIFEST_ENTRIES) return -1;
    /* u64 total_bytes (informational; recomputed below) */ (void)get_u64(&p);
    SyncFileMeta *files = (SyncFileMeta *)calloc(count ? count : 1, sizeof(SyncFileMeta));
    if (!files) return -1;
    for (uint32_t i = 0; i < count; i++) {
        SyncFileMeta *f = &files[i];
        if ((size_t)(p - data) + 4 > n) { free(files); return -1; }
        f->kind = *p++; f->in_playlist = *p++; p += 2;
        if ((size_t)(p - data) + 8 + 8 + 4 + 32 > n) { free(files); return -1; }
        f->size = get_u64(&p); f->mtime_ns = get_u64(&p);
        uint32_t nb = get_u32(&p);
        if (nb != (f->size + SYNC_BLOCK - 1) / SYNC_BLOCK) { free(files); return -1; }
        if (nb > (uint64_t)SYNC_MAX_FILE_BLOCKS) { free(files); return -1; }
        memcpy(f->sha, p, 32); p += 32;
        if ((size_t)(p - data) + 2 > n) { free(files); return -1; }
        uint16_t rl = get_u16(&p);
        if (rl > SYNC_MAX_NAME || (size_t)(p - data) + rl + 2 > n) { free(files); return -1; }
        memcpy(f->rel_path, p, rl); p += rl; f->rel_path[rl] = 0;
        uint16_t pl = get_u16(&p);
        if (pl > SYNC_MAX_NAME || (size_t)(p - data) + pl > n) { free(files); return -1; }
        memcpy(f->playlist_ref, p, pl); p += pl; f->playlist_ref[pl] = 0;
        if (nb) {
            if ((size_t)(p - data) + (size_t)nb * 4 > n) { free(files); return -1; }
            f->block_crcs = (uint32_t *)malloc((size_t)nb * 4);
            if (!f->block_crcs) { free(files); return -1; }
            for (uint32_t b = 0; b < nb; b++) f->block_crcs[b] = get_u32(&p);
        }
    }
    if ((size_t)(p - data) != n) {   // trailing garbage => truncated/corrupt
        for (uint32_t i = 0; i < count; i++) free(files[i].block_crcs);
        free(files); return -1;
    }
    m->files = files; m->count = count;
    sha256(data, n, m->id);
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) total += files[i].size;
    m->total_bytes = total;
    return 0;
}
void manifest_free(SyncManifest *m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->count; i++) free(m->files[i].block_crcs);
    free(m->files);
    m->files = NULL; m->count = 0;
}
// sender-side helper: compute per-block CRCs for every file of the plan
int manifest_compute_crcs(SyncManifest *m, const SyncEngine *eng) {
    if (!m || !eng || !eng->read_source) return -1;
    uint8_t *buf = (uint8_t *)malloc(SYNC_BLOCK);
    if (!buf) return -1;
    for (uint32_t i = 0; i < m->count; i++) {
        SyncFileMeta *f = &m->files[i];
        uint64_t nb = (f->size + SYNC_BLOCK - 1) / SYNC_BLOCK;
        if (nb > (uint64_t)SYNC_MAX_FILE_BLOCKS) { free(buf); return -1; }
        free(f->block_crcs);
        f->block_crcs = nb ? (uint32_t *)malloc((size_t)nb * 4) : NULL;
        if (nb && !f->block_crcs) { free(buf); return -1; }
        for (uint64_t b = 0; b < nb; b++) {
            uint64_t off = b * SYNC_BLOCK;
            size_t take = f->size - off > SYNC_BLOCK ? SYNC_BLOCK : (size_t)(f->size - off);
            int got = eng->read_source(eng->user, (int)i, off, buf, take);
            if (got != (int)take) { free(buf); return -2; }  // source changed
            f->block_crcs[b] = sync_crc32c(0, buf, take);
        }
    }
    free(buf);
    return 0;
}
