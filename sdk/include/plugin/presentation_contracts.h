#pragma once
#include <stdint.h>
#define LYRA_INTERFACE_PRESENTATION_CONTEXT 17u
typedef struct LyraPresentationRectV3 {
  float x, y, w, h, radius;
} LyraPresentationRectV3;
typedef struct LyraPresentationSnapshotV3 {
  uint32_t size;
  uint64_t revision;
  int64_t frame_ns;
  int32_t width, height;
  float density, font_scale;
  LyraPresentationRectV3 card, cover, title, artist, lyrics, library, search,
      settings, sheet;
  float theme_top[4], theme_bottom[4];
} LyraPresentationSnapshotV3;
typedef struct LyraResourceInfoV3 {
  uint32_t size;
  int32_t width, height, ready;
  // Image pixels are borrowed until the next frame boundary/cache mutation.
  const uint8_t *rgba;
  uint64_t bytes;
} LyraResourceInfoV3;
struct LyraInput;
// Frame-thread reads, by value. Cache identities come from media/scene
// descriptors.
typedef struct LyraPresentationContextV3 {
  uint32_t size, version;
  void *user;
  int (*snapshot)(void *, LyraPresentationSnapshotV3 *);
  int (*glyph)(void *, const char *, LyraResourceInfoV3 *);
  int (*image)(void *, int64_t, LyraResourceInfoV3 *);
  int (*input)(void *, const struct LyraInput *, int present);
} LyraPresentationContextV3;
