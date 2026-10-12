#pragma once

#include <stdint.h>

#include "plugin/contracts.h"

#define LYRA_PLUGIN_ABI 3

#define LYRA_PERM_GRANTED 0
#define LYRA_PERM_DENIED 1
#define LYRA_PERM_UNDECLARED 2
#define LYRA_PERM_UNAVAILABLE 3

#define LYRA_INPUT_DOWN 0
#define LYRA_INPUT_UP 1
#define LYRA_INPUT_MOVE 2
#define LYRA_INPUT_CANCEL 3

typedef struct LyraInput {
    uint32_t size;
    int32_t action;
    float x;
    float y;
    int64_t time_ms;
} LyraInput;

typedef struct LyraSolid {
    float x, y, w, h, radius, r, g, b, a;
} LyraSolid;

typedef struct LyraCoverCmd {
    int64_t key;
    float x, y, w, h, radius;
} LyraCoverCmd;

typedef struct LyraIconCmd {
    int32_t index;
    float x, y, w, h, r, g, b, a;
} LyraIconCmd;

typedef struct LyraVeilCmd {
    float x, y, w, h, edge, dir, r, g, b, a;
} LyraVeilCmd;

typedef struct LyraFlowCmd {
    float x, y, w, h, radius;
} LyraFlowCmd;

typedef struct LyraImageCmd {
    float x, y, w, h;
    int32_t width, height;
    const uint8_t *rgba;
} LyraImageCmd;

typedef struct LyraBox {
    uint32_t size;
    float x, y, w, h;
} LyraBox;

typedef void (*LyraBoxFn)(void *user, LyraBox *box);

typedef struct LyraServiceView {
    uint32_t size;
    char id[64];
    char capability[24];
    char version[32];
    char config[64];
    int32_t external;
    int32_t active;
    int32_t status; // 0 usable, 1 unsupported ABI; recompile required
} LyraServiceView;

typedef struct LyraPanes {
    uint32_t size;
    float lib_x, lib_y, lib_w, lib_h;
    float set_x, set_y, set_w, set_h;
    float search_x, search_y, search_w, search_h;
    float h_off, v_off;
    float sheet_x, sheet_y, sheet_w, sheet_h;
    float s_off;
} LyraPanes;

// Optional effect on the submitted scene. Center is normalized to its card.
typedef struct LyraRippleCmd {
    float center_x, center_y, progress, pinch, strength;
} LyraRippleCmd;

// Semantic layers reuse the host's text/shape effects, without exposing GLES handles.
#define LYRA_LAYER_SWEEP 1u
#define LYRA_LAYER_HALO 2u
#define LYRA_LAYER_CLIP 4u
#define LYRA_LAYER_FADE 8u
typedef struct LyraTextLayerV3 {
    uint32_t size, flags;
    const char *text, *font;
    float x, y, w, h, text_size, r, g, b, a;
    float opacity, blur_px;
    float sweep_row, sweep_rows, sweep_cut, sweep_direction;
    float clip_x, clip_y, clip_w, clip_h, clip_radius;
    float fade_left, fade_right, fade_edge;
} LyraTextLayerV3;
#define LYRA_MATERIAL_FLAT 0u
#define LYRA_MATERIAL_GRADIENT 1u
#define LYRA_MATERIAL_RIM 2u
typedef struct LyraShapeLayerV3 {
    uint32_t size, material, flags;
    LyraSolid shape;
    float r2, g2, b2, a2, edge_px;
    float clip_x, clip_y, clip_w, clip_h, clip_radius;
} LyraShapeLayerV3;

typedef struct LyraSceneV3 {
    uint32_t size;
    const LyraSolid *solids;
    int32_t solid_count;
    const LyraCoverCmd *covers;
    int32_t cover_count;
    const LyraIconCmd *icons;
    int32_t icon_count;
    const LyraVeilCmd *veils;
    int32_t veil_count;
    const LyraFlowCmd *flow;
    int32_t spare;
    const LyraImageCmd *images;
    int32_t image_count;
    const LyraRippleCmd *ripple;
    const LyraTextLayerV3 *text_layers;
    int32_t text_layer_count;
    const LyraShapeLayerV3 *shape_layers;
    int32_t shape_layer_count;
} LyraSceneV3;

typedef struct LyraHostV3 LyraHostV3;
typedef struct LyraPluginV3 LyraPluginV3;

struct LyraHostV3 {
    uint32_t size;
    uint32_t abi;
    void *user;
    void (*log)(void *user, const char *message);
    void (*request_frame)(void *user);
    int64_t (*now_ns)(void *user);
    float (*density)(void *user);
    void (*submit)(void *user, const LyraSceneV3 *scene);
    void (*layer)(void *user, const char *id, float z, float x, float y, float w, float h);
    int (*config_text)(void *user, const char *table, char *out, int cap);
    int (*request_permission)(void *user, const char *name);
    void (*set_panes)(void *user, const LyraPanes *panes);
    int (*acquire_surface)(void *user, const char *name, int32_t w, int32_t h);
    int (*section)(void *user, const char *capability, float *x, float *y, float *w, float *h);
    void (*listen)(void *user, const char *event, LyraBoxFn fn, void *fn_user);
    void *java_vm;
    void *android_context;
    const void *(*query_interface)(void *, uint32_t, uint32_t, uint32_t);
};

struct LyraPluginV3 {
    uint32_t size;
    uint32_t abi;
    const char *id;
    const char *kind;
    const char *version;
    const char *config_tables;
    int (*init)(const LyraHostV3 *host);
    int (*bind)(const LyraHostV3 *host);
    void (*shutdown)(const LyraHostV3 *host);
    int (*on_frame)(const LyraHostV3 *host, int64_t time_ns);
    int (*on_input)(const LyraHostV3 *host, const LyraInput *input);
    void (*on_permission)(const LyraHostV3 *host, const char *name, int status);
    int (*supply)(const LyraHostV3 *host, int64_t id, char *buf, int cap);
    const void *(*query_interface)(const LyraHostV3 *, uint32_t, uint32_t, uint32_t);
};

typedef int (*LyraPluginInitFn)(const LyraHostV3 *host, LyraPluginV3 *out);
