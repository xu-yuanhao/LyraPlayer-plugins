// sync_db.h - persistent task database (append-log + snapshot, crash-safe).
#pragma once
#include "sync_common.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct SyncTaskDb SyncTaskDb;

// One persisted file entry. task_id groups entries of one sync plan; the plan
// is identified by (peer device id, manifest hash) so reconnects reuse it.
typedef struct SyncDbEntry {
    uint64_t task_id;
    uint32_t index;
    uint8_t xfer_state;   // XS_*
    uint64_t received_bytes;   // validated bytes only
    uint32_t have_blocks;      // validated 256KiB block count
    char rel_path[SYNC_MAX_NAME + 1];
    uint8_t sha[32];
    uint64_t size;
    uint8_t kind;
} SyncDbEntry;

// task_state persisted per task
typedef struct SyncDbTask {
    uint64_t task_id;
    uint8_t state;        // TS_*
    char peer_device[65]; // sha256 hex of peer identity
    char manifest_id[65]; // sha256 hex of manifest
    uint64_t total_bytes;
    uint64_t confirmed_bytes;
} SyncDbTask;

SyncTaskDb *sync_db_open(const char *dir);
void sync_db_close(SyncTaskDb *db);
// upsert task row + entry rows (batched txn)
int sync_db_put_task(SyncTaskDb *, const SyncDbTask *);
int sync_db_put_entries(SyncTaskDb *, const SyncDbEntry *, int count);
int sync_db_load_all(SyncTaskDb *,
                     int (*task_fn)(void *, const SyncDbTask *), void *tuser,
                     int (*entry_fn)(void *, const SyncDbEntry *), void *euser);
int sync_db_delete_task(SyncTaskDb *, uint64_t task_id);

#ifdef __cplusplus
}
#endif