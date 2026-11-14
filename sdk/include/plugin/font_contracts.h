#pragma once
#include "message_contracts.h"
#define LYRA_INTERFACE_FONT_MANAGER 25u
// Frame-thread installed-font management on the selected font service.
// REMOVE requires a NUL-terminated choice ID, never a filesystem path. Only
// imported families may be removed. Reply SELECTED is I64 (0/1), so callers can
// persist automatic selection when removing the active family. All family
// weights are removed together. Malformed requests have no filesystem effects.
enum {LYRA_FONT_REMOVE=1,LYRA_FONT_CHOICE=1,LYRA_FONT_SELECTED=2};
typedef struct LyraFontManagerV3 {
    uint32_t size,version;
    void *user;
    int (*request)(void *,const LyraMessageV3 *,LyraReplyV3 *);
} LyraFontManagerV3;
