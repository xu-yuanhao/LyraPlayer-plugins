#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_PERSONAL_LIBRARY 21u
// Frame-thread, tagged operations. LIST returns a revision and UTF-8 JSON
// snapshot. Playlist/track IDs are I64; names and snapshots include a trailing
// UTF-8 NUL. CREATE rejects empty/duplicate names. DISSOLVE never deletes audio
// files. DELETE_FILE initiates Android's consent flow; POLL_DELETE returns the
// confirmed removed track ID once (0 pending, negative ID cancelled/failed).
// PLAY_NEXT and BLOCK_FOLDER take TRACK. They keep playback running.
// BLOCK_FOLDER persists the track directory in the existing scan policy.
// Storage errors never commit.
enum {
  LYRA_PERSONAL_LIST = 1,
  LYRA_PERSONAL_CREATE = 2,
  LYRA_PERSONAL_DISSOLVE = 3,
  LYRA_PERSONAL_ADD = 4,
  LYRA_PERSONAL_DELETE_FILE = 5,
  LYRA_PERSONAL_POLL_DELETE = 6,
  LYRA_PERSONAL_SET_INPUT = 7,
  LYRA_PERSONAL_PLAY_NEXT = 8,
  LYRA_PERSONAL_BLOCK_FOLDER = 9,
  // ID and TRACK are playback entry handles for this operation. TRACK=0 removes ID;
  // otherwise move ID to TRACK's visible queue position. Never deletes a file.
  LYRA_PERSONAL_EDIT_QUEUE = 10
};
enum {
  LYRA_PERSONAL_ID = 1,
  LYRA_PERSONAL_NAME = 2,
  LYRA_PERSONAL_TRACK = 3,
  LYRA_PERSONAL_REVISION = 4,
  LYRA_PERSONAL_SNAPSHOT = 5
};
typedef struct LyraPersonalLibraryV3 {
  uint32_t size, version;
  void *user;
  int (*request)(void *, const LyraMessageV3 *, LyraReplyV3 *);
} LyraPersonalLibraryV3;
enum {
  // ADD may take PROVIDER + UID instead of TRACK. Both are nonempty UTF-8
  // including the terminator; provider must not contain a newline. No resource
  // URLs or temporary collection/queue handles are persisted.
  LYRA_PERSONAL_PROVIDER = 6,
  LYRA_PERSONAL_UID = 7
};

// ADD with PROVIDER and UID may also supply an optional NAME as a cached song title.
