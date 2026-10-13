#pragma once
// Pure C++17 helpers for the Netease free-music plugin.
// No JNI, no OS calls: this file is compiled for the host unit test too.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace netease {

struct SongInfo {
    std::string id;        // decimal song id
    std::string title;
    std::string artist;    // blended with "/"
    std::string album;
    int64_t duration_ms = 0;
    std::string cover;     // raw cover url (may be http)
    int fee = 0;
};

// Parse a cloudsearch (`result.songs`) or song/detail (`songs`) payload.
// Appends free (fee == 0) songs, at most max_results. Returns appended
// count, or -1 when the payload has no usable song array.
int ParseSongs(const char *json, size_t len, std::vector<SongInfo> &out,
               size_t max_results);

// Parse a song/lyric payload into merged LRC text (translation lines are
// interleaved after their original line). Empty output means no usable
// lyric. Returns 0 on syntactically valid JSON, -1 otherwise.
int ParseLyric(const char *json, size_t len, std::string &lrc_out);

// Rewrite http://*.music.126.net to https:// (verified reachable).
// Other urls are returned unchanged.
std::string ToHttps126(const std::string &url);

// Percent-encode a UTF-8 query for the ?s= parameter.
std::string UrlEncode(const std::string &in);

bool IsDigits(const std::string &s);

}  // namespace netease
