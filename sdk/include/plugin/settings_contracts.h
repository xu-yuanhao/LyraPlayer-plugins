#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_SETTINGS_PAGE 26u
// Optional on any selected service; frame thread only. The host enumerates
// endpoints without private names. All scalar fields use I64/F64, text uses
// UTF8 including NUL, data borrowed until next request. Unknown required fields
// are rejected; each input tag occurs once. INFO is side-effect free.
// ENTRY_PARENT optionally selects a stable host section path; absent means root.
// Unknown paths are not silently moved to root. This is presentation metadata,
// independent of service names and plugin identity.
// INFO: TITLE (page heading), ENTRY_TITLE (stable main settings label), COUNT, PAGE, REVISION, optional PROMPT/NOTICE.
// ROW(INDEX): TITLE, optional DETAIL/BUTTON, FLAGS, optional VALUE/MIN/MAX/STEP.
// OPEN resets navigation to PAGE 0; BACK returns UNSUPPORTED at root, OK otherwise.
// ACTIVATE(INDEX, optional SECONDARY or VALUE) uses current row identities.
// CONFIRM(ACCEPT) resolves the current prompt; BACK cancels a prompt first.
// On the host endpoint, HOST_CAPABILITIES returns supported row and prompt FLAGS; OPEN
// requires HANDLE (HANDLE field type) of a selected settings endpoint.
// UI owns scroll/slider preview; plugin owns validation, actions and persistence.
enum {LYRA_SETTINGS_INFO=1,LYRA_SETTINGS_ROW=2,LYRA_SETTINGS_OPEN=3,
      LYRA_SETTINGS_BACK=4,LYRA_SETTINGS_ACTIVATE=5,LYRA_SETTINGS_CONFIRM=6,
      LYRA_SETTINGS_HOST_CAPABILITIES=7};
enum {LYRA_SETTINGS_TITLE=1,LYRA_SETTINGS_COUNT=2,LYRA_SETTINGS_PAGE=3,
      LYRA_SETTINGS_REVISION=4,LYRA_SETTINGS_PROMPT=5,LYRA_SETTINGS_NOTICE=6,
      LYRA_SETTINGS_INDEX=7,LYRA_SETTINGS_DETAIL=8,LYRA_SETTINGS_BUTTON=9,
      LYRA_SETTINGS_FLAGS=10,LYRA_SETTINGS_VALUE=11,LYRA_SETTINGS_MINIMUM=12,
      LYRA_SETTINGS_MAXIMUM=13,LYRA_SETTINGS_STEP=14,LYRA_SETTINGS_SECONDARY=15,
      LYRA_SETTINGS_ACCEPT=16,LYRA_SETTINGS_HANDLE=17,LYRA_SETTINGS_ENTRY_TITLE=18};
enum {LYRA_SETTINGS_TOGGLE=1,LYRA_SETTINGS_CHECKED=2,LYRA_SETTINGS_DISABLED=4,
      LYRA_SETTINGS_GROUP_END=8,LYRA_SETTINGS_SLIDER=16,LYRA_SETTINGS_FORWARD=32};
typedef struct LyraSettingsPageV3 {
 uint32_t size,version;
 void *user;
 int (*request)(void *,const LyraMessageV3 *,LyraReplyV3 *);
} LyraSettingsPageV3;

// Optional INFO placement metadata; registered in the interface ledger.
enum {LYRA_SETTINGS_ENTRY_PARENT=19};
// Optional INFO fields for a single-line prompt. Presence of INPUT (even an
// empty string) enables editing. CONFIRM echoes the edited INPUT on accept.
// PROMPT_ACTION labels a third button; ACTIVATE_PROMPT preserves the draft.
enum {LYRA_SETTINGS_INPUT=20, LYRA_SETTINGS_PROMPT_ACTION=21};
enum {LYRA_SETTINGS_ACTIVATE_PROMPT=8};
// These bits are returned by HOST_CAPABILITIES only.
enum {LYRA_SETTINGS_CAN_EDIT_PROMPT=64, LYRA_SETTINGS_CAN_ACTIVATE_PROMPT=128};
// Optional ROW UTF-8 label for a leading tile. Omit for a plain row.
enum {LYRA_SETTINGS_LEADING_TEXT=22};
enum {LYRA_SETTINGS_CAN_LEAD_TEXT=256};
// Optional ROW bytes: tightly packed, premultiplied RGBA, top row first.
enum {LYRA_SETTINGS_LEADING_RGBA=23};
enum {LYRA_SETTINGS_CAN_LEAD_RGBA=512};
enum {LYRA_SETTINGS_TILE_SIDE=32};
// INFO returns an I64 nonzero value to mount its root rows in ENTRY_PARENT.
enum {LYRA_SETTINGS_INLINE=24};
enum {LYRA_SETTINGS_CAN_MOUNT_ROWS=1024};
