#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace flyme {
struct Cue { int64_t start, end; std::string text; };
// Latest active primary line wins in a duet. Never display future cues or
// leave the previous track's lyric visible in an instrumental gap.
inline const char *current(const std::vector<Cue> &cues, int64_t position) {
    const Cue *selected = nullptr;
    for (const auto &cue : cues) {
        if (cue.start <= position && position < cue.end &&
            (!selected || cue.start >= selected->start)) selected = &cue;
    }
    return selected ? selected->text.c_str() : "";
}
}
