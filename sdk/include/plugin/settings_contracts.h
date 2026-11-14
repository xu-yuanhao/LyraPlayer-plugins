#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_SETTINGS_PAGE 26u
// Optional on any selected service; frame thread only. The host enumerates
// endpoints without private names. All scalar fields use I64/F64, text uses
// UTF8 including NUL, data borrowed until next request. Unknown required fields
// are rejected; each input tag occurs once. INFO is side-effect free.
// INFO: TITLE (page heading), ENTRY_TITLE (stable main settings label), COUNT, PAGE, REVISION, optional PROMPT/NOTICE.
// ROW(INDEX): TITLE, optional DETAIL/BUTTON, FLAGS, optional VALUE/MIN/MAX/STEP.
// OPEN resets navigation to PAGE 0; BACK returns UNSUPPORTED at root, OK otherwise.
// ACTIVATE(INDEX, optional SECONDARY or VALUE) uses current row identities.
// CONFIRM(ACCEPT) resolves the current prompt; BACK cancels a prompt first.
// On the host endpoint, HOST_CAPABILITIES returns supported row FLAGS; OPEN
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
