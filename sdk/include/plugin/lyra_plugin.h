#pragma once

#include <stdint.h>

#include "plugin/contracts.h"

#define LYRA_PLUGIN_ABI 2

#define LYRA_PERM_GRANTED 0
#define LYRA_PERM_DENIED 1
#define LYRA_PERM_UNDECLARED 2
#define LYRA_PERM_UNAVAILABLE 3

#define LYRA_INPUT_DOWN 0
#define LYRA_INPUT_UP 1
#define LYRA_INPUT_MOVE 2
#define LYRA_INPUT_CANCEL 3

typedef struct LyraRecord {
    uint32_t size;
    int64_t id;
    int64_t album_id;
    int64_t duration_ms;
    char title[80];
    char artist[80];
} LyraRecord;

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

typedef struct LyraGlyphCmd {
    float x, y, size, r, g, b, a;
    const char *text;
    const char *font;
} LyraGlyphCmd;

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
typedef struct LyraTextLayerV2 {
    uint32_t size, flags;
    const char *text, *font;
    float x, y, w, h, text_size, r, g, b, a;
    float opacity, blur_px;
    float sweep_row, sweep_rows, sweep_cut, sweep_direction;
    float clip_x, clip_y, clip_w, clip_h, clip_radius;
    float fade_left, fade_right, fade_edge;
} LyraTextLayerV2;
#define LYRA_MATERIAL_FLAT 0u
#define LYRA_MATERIAL_GRADIENT 1u
#define LYRA_MATERIAL_RIM 2u
typedef struct LyraShapeLayerV2 {
    uint32_t size, material, flags;
    LyraSolid shape;
    float r2, g2, b2, a2, edge_px;
    float clip_x, clip_y, clip_w, clip_h, clip_radius;
} LyraShapeLayerV2;

typedef struct LyraSceneV2 {
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
    const LyraGlyphCmd *glyphs;
    int32_t glyph_count;
    int32_t spare;
    const LyraImageCmd *images;
    int32_t image_count;
    const LyraRippleCmd *ripple;
    const LyraTextLayerV2 *text_layers;
    int32_t text_layer_count;
    const LyraShapeLayerV2 *shape_layers;
    int32_t shape_layer_count;
} LyraSceneV2;

typedef struct LyraHostV2 LyraHostV2;
typedef struct LyraPluginV2 LyraPluginV2;

struct LyraHostV2 {
    uint32_t size;
    uint32_t abi;
    void *user;
    void (*log)(void *user, const char *message);
    void (*request_frame)(void *user);
    int64_t (*now_ns)(void *user);
    float (*density)(void *user);
    void (*submit)(void *user, const LyraSceneV2 *scene);
    void (*layer)(void *user, const char *id, float z, float x, float y, float w, float h);
    int (*library_query)(void *user, const char *text, LyraRecord *out, int cap);
    int (*search_query)(void *user, const char *text, LyraRecord *out, int cap);
    int (*config_text)(void *user, const char *table, char *out, int cap);
    int (*request_permission)(void *user, const char *name);
    // 0 toggle, 1 seek(ms), 2 position(ms), 3 set mode, 4 get mode.
    // 5 queue count, 6 queue track catalog index(ms=row), 7 play queue row, 8 current queue row.
    int (*transport)(void *user, int action, int64_t ms, int64_t *position);
    const LyraPluginV2 *(*find)(void *user, const char *capability);
    void (*set_panes)(void *user, const LyraPanes *panes);
    int (*acquire_surface)(void *user, const char *name, int32_t w, int32_t h);
    int (*section)(void *user, const char *capability, float *x, float *y, float *w, float *h);
    void (*listen)(void *user, const char *event, LyraBoxFn fn, void *fn_user);
    void *java_vm;
    void *android_context;
    const void *(*query_interface)(void *, uint32_t, uint32_t, uint32_t);
};

struct LyraPluginV2 {
    uint32_t size;
    uint32_t abi;
    const char *id;
    const char *kind;
    const char *version;
    const char *config_tables;
    int (*init)(const LyraHostV2 *host);
    int (*bind)(const LyraHostV2 *host);
    void (*shutdown)(const LyraHostV2 *host);
    int (*on_frame)(const LyraHostV2 *host, int64_t time_ns);
    int (*on_input)(const LyraHostV2 *host, const LyraInput *input);
    int (*query)(const LyraHostV2 *host, const char *text, LyraRecord *out, int cap);
    void (*on_permission)(const LyraHostV2 *host, const char *name, int status);
    int (*open_fd)(const LyraHostV2 *host, int64_t id);
    int (*supply)(const LyraHostV2 *host, int64_t id, char *buf, int cap);
    const void *(*query_interface)(const LyraHostV2 *, uint32_t, uint32_t, uint32_t);
};

typedef int (*LyraPluginInitFn)(const LyraHostV2 *host, LyraPluginV2 *out);

#ifdef __cplusplus
extern "C" {
#endif

const LyraHostV2 *lyra_plugin_host();
const void *lyra_plugin_interface(const char *, uint32_t, uint32_t, uint32_t);
const LyraPluginV2 *lyra_plugin_find(const char *id);
void lyra_plugin_startup(void);
void lyra_plugin_set_roots(const char *external_root, const char *internal_root);
void lyra_plugin_refresh(void);
struct Scene;
void lyra_plugin_frame(struct Scene *scene, int64_t time_ns);
void lyra_plugin_unbind(void);
int lyra_plugin_input(int action, float x, float y, int64_t time_ms);
int lyra_plugin_owns_config(const char *table);
void lyra_plugin_permission(const char *name, int status);
int lyra_plugin_any(void);
void lyra_plugin_provide(const char *name, const struct LyraPluginV2 *plugin);
void lyra_plugin_set_root(const char *name);
const struct LyraPluginV2 *lyra_plugin_service(const char *name);
int lyra_plugin_external_name(const char *name);
void lyra_service_invoke(const char *name, float x, float y, float w, float h);
int lyra_emit_box(const char *event, LyraBox *box);
void lyra_plugin_use_queries(const struct LyraPluginV2 *catalog, const struct LyraPluginV2 *search);
int lyra_plugin_supply(const char *name, int64_t id, char *buf, int cap);
int lyra_plugin_hit(int action, float x, float y, int64_t time_ms);
struct Scene *lyra_frame_scene(void);
struct Scene *lyra_frame_target(struct Scene *scene);
int lyra_plugin_copy_panes(LyraPanes *out);
int lyra_plugin_open_fd(int64_t id);
int lyra_plugin_query(const char *capability, const char *text, LyraRecord *out, int cap);
int lyra_plugin_services(LyraServiceView *out, int cap);
int lyra_plugin_inventory(LyraServiceView *out, int cap, int external);
int lyra_service_input(const char *name, const LyraInput *input);
struct Scene *lyra_plugin_extra_scene(void);

struct LyraBridge {
    void *user;
    void (*request_frame)(void *user);
    float (*density)(void *user);
    int (*query)(void *user, int filtered, const char *text, LyraRecord *out, int cap);
    int (*config)(void *user, const char *table, char *out, int cap);
    int (*permit)(void *user, const char *name);
    void (*toggle)(void *user);
    void (*seek)(void *user, int64_t ms);
    int64_t (*position)(void *user);
    int (*acquire_surface)(void *user, const char *name, int w, int h);
    int (*transport)(void *user, int action, int64_t ms, int64_t *position);
    const void *(*query_interface)(void *, uint32_t, uint32_t, uint32_t);
    int (*before_unload)(void *, const LyraPluginV2 *);
};
typedef struct LyraBridge LyraBridge;
void lyra_plugin_set_bridge(const LyraBridge *bridge);
void lyra_plugin_set_platform(void *java_vm, void *android_context);

#ifdef __cplusplus
}
#endif
