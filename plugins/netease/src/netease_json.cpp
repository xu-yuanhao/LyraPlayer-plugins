// Minimal JSON scanning for the two Netease payloads the plugin consumes.
// Only standard headers; no exceptions are thrown (allocation failure aborts
// under -fno-exceptions, same as the rest of the plugin).
#include "netease_json.h"

#include <cstdio>
#include <cstring>

#include "crypto.h"

namespace netease {
namespace {

const size_t kNpos = static_cast<size_t>(-1);
const size_t kFieldCap = 512;
const size_t kLyricCap = 1u << 20;  // 1 MiB, mirrors the host text budget.

size_t SkipWs(const char *s, size_t n, size_t i) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    return i;
}

void AppendUtf8(uint32_t cp, std::string &out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

unsigned HexVal(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    return 16;
}

// s[i] must be '"'. Unescapes into out, returns the index past the closing
// quote, or kNpos on truncation.
size_t ParseJsonString(const char *s, size_t n, size_t i, std::string &out) {
    out.clear();
    ++i;  // opening quote
    while (i < n) {
        char c = s[i];
        if (c == '"') return i + 1;
        if (c != '\\') {
            out.push_back(c);
            ++i;
            continue;
        }
        if (++i >= n) return kNpos;
        char e = s[i++];
        switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (i + 4 > n) return kNpos;
                uint32_t cp = 0;
                for (int k = 0; k < 4; ++k) {
                    unsigned v = HexVal(s[i + k]);
                    if (v > 15) return kNpos;
                    cp = (cp << 4) | v;
                }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= n && s[i] == '\\' &&
                    s[i + 1] == 'u') {
                    uint32_t lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; ++k) {
                        unsigned v = HexVal(s[i + 2 + k]);
                        if (v > 15) {
                            ok = false;
                            break;
                        }
                        lo = (lo << 4) | v;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                AppendUtf8(cp, out);
                break;
            }
            default: return kNpos;
        }
        if (out.size() > kLyricCap) return kNpos;
    }
    return kNpos;
}

// Find "key" outside string literals. want_depth < 0 matches at any depth;
// otherwise only direct members at that depth (relative to pos) match, so a
// nested member such as al.name cannot shadow the song's own name.
// Returns the index of its opening quote.
size_t FindKeyAt(const char *s, size_t n, size_t pos, const char *key,
                 int want_depth) {
    size_t klen = std::strlen(key);
    bool in_str = false;
    int depth = 0;
    for (size_t i = pos; i < n; ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            if ((want_depth < 0 || depth == want_depth) &&
                i + klen + 1 < n &&
                std::memcmp(s + i + 1, key, klen) == 0 &&
                s[i + klen + 1] == '"') {
                return i;
            }
            in_str = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            if (depth == 0) return kNpos;
            --depth;
        }
    }
    return kNpos;
}

// Direct members of the object/array span starting at pos.
size_t FindKey(const char *s, size_t n, size_t pos, const char *key) {
    return FindKeyAt(s, n, pos, key, 1);
}

// q is the opening quote of a key. Returns the value start (after ':').
size_t ColonValue(const char *s, size_t n, size_t q, size_t klen) {
    size_t i = SkipWs(s, n, q + klen + 2);
    if (i >= n || s[i] != ':') return kNpos;
    return SkipWs(s, n, i + 1);
}

// s[i] is '{' or '['. Returns the index past the matching closer.
size_t MatchBracket(const char *s, size_t n, size_t i) {
    char open = s[i], close = (open == '{') ? '}' : ']';
    int depth = 0;
    bool in_str = false;
    for (; i < n; ++i) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') {
                ++i;
                continue;
            }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == open) {
            ++depth;
        } else if (c == close) {
            if (--depth == 0) return i + 1;
        }
    }
    return kNpos;
}

long long ParseNum(const char *s, size_t n, size_t i, size_t *end) {
    i = SkipWs(s, n, i);
    bool neg = false;
    if (i < n && s[i] == '-') {
        neg = true;
        ++i;
    }
    long long v = 0;
    bool any = false;
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        ++i;
        any = true;
    }
    if (!any) return 0;
    if (end) *end = i;
    return neg ? -v : v;
}

// Extract a string field from an object substring; empty when absent.
std::string FieldStr(const char *s, size_t n, const char *key) {
    size_t q = FindKey(s, n, 0, key);
    if (q == kNpos) return std::string();
    size_t v = ColonValue(s, n, q, std::strlen(key));
    if (v == kNpos || v >= n || s[v] != '"') return std::string();
    std::string out;
    if (ParseJsonString(s, n, v, out) == kNpos) return std::string();
    if (out.size() > kFieldCap) out.resize(kFieldCap);
    return out;
}

long long FieldNum(const char *s, size_t n, const char *key, bool *found) {
    size_t q = FindKey(s, n, 0, key);
    if (q == kNpos) {
        if (found) *found = false;
        return 0;
    }
    size_t v = ColonValue(s, n, q, std::strlen(key));
    if (v == kNpos || v >= n) {
        if (found) *found = false;
        return 0;
    }
    if (s[v] == '"') {  // some endpoints quote numbers
        std::string t;
        if (ParseJsonString(s, n, v, t) == kNpos) {
            if (found) *found = false;
            return 0;
        }
        if (found) *found = true;
        long long r = 0;
        bool neg = false;
        for (char c : t) {
            if (c == '-' && r == 0) {
                neg = true;
                continue;
            }
            if (c < '0' || c > '9') break;
            r = r * 10 + (c - '0');
        }
        return neg ? -r : r;
    }
    size_t end = v;
    long long r = ParseNum(s, n, v, &end);
    if (found) *found = (end != v);
    return r;
}

// Collect "name" from every object inside an array field (artists).
std::string FieldNameList(const char *s, size_t n, const char *key) {
    std::string out;
    size_t q = FindKey(s, n, 0, key);
    if (q == kNpos) return out;
    size_t v = ColonValue(s, n, q, std::strlen(key));
    if (v == kNpos || v >= n || s[v] != '[') return out;
    size_t arr_end = MatchBracket(s, n, v);
    if (arr_end == kNpos) return out;
    size_t i = v + 1;
    while (i < arr_end) {
        i = SkipWs(s, n, i);
        if (i >= arr_end || s[i] != '{') {
            ++i;
            continue;
        }
        size_t obj_end = MatchBracket(s, n, i);
        if (obj_end == kNpos || obj_end > arr_end) break;
        std::string name = FieldStr(s + i, obj_end - i, "name");
        if (!name.empty()) {
            if (!out.empty()) out += "/";
            out += name;
            if (out.size() > kFieldCap) {
                out.resize(kFieldCap);
                break;
            }
        }
        i = obj_end;
    }
    return out;
}

// Extract a sub-object field ("al"/"album") into [begin, end).
bool FieldObject(const char *s, size_t n, const char *key, size_t *begin,
                 size_t *end) {
    size_t q = FindKey(s, n, 0, key);
    if (q == kNpos) return false;
    size_t v = ColonValue(s, n, q, std::strlen(key));
    if (v == kNpos || v >= n || s[v] != '{') return false;
    size_t e = MatchBracket(s, n, v);
    if (e == kNpos) return false;
    *begin = v;
    *end = e;
    return true;
}

bool ValidLrc(const std::string &l) {
    if (l.empty() || l.compare(0, 4, "http") == 0) return false;
    return l.find('[') != std::string::npos &&
           l.find(']') != std::string::npos;
}

}  // namespace

int ParseSongs(const char *json, size_t len, std::vector<SongInfo> &out,
               size_t max_results) {
    if (!json || len == 0 || max_results == 0) return len == 0 ? -1 : 0;
    size_t q = FindKeyAt(json, len, 0, "songs", -1);
    if (q == kNpos) return -1;
    size_t v = ColonValue(json, len, q, 5);
    if (v == kNpos || v >= len || json[v] != '[') return -1;
    size_t arr_end = MatchBracket(json, len, v);
    if (arr_end == kNpos) return -1;
    int appended = 0;
    size_t i = v + 1;
    while (i < arr_end && out.size() < max_results) {
        i = SkipWs(json, len, i);
        if (i >= arr_end) break;
        if (json[i] != '{') {
            ++i;
            continue;
        }
        size_t obj_end = MatchBracket(json, len, i);
        if (obj_end == kNpos || obj_end > arr_end) break;
        const char *o = json + i;
        size_t on = obj_end - i;
        bool has_fee = false;
        long long fee = FieldNum(o, on, "fee", &has_fee);
        if (has_fee && fee != 0) {
            i = obj_end;  // skip VIP/paid tracks, keep them out of results
            continue;
        }
        bool has_id = false;
        long long id = FieldNum(o, on, "id", &has_id);
        if (!has_id || id <= 0) {
            i = obj_end;
            continue;
        }
        SongInfo song;
        char idbuf[32];
        std::snprintf(idbuf, sizeof(idbuf), "%lld", id);
        song.id = idbuf;
        song.fee = has_fee ? static_cast<int>(fee) : 0;
        song.title = FieldStr(o, on, "name");
        song.artist = FieldNameList(o, on, "ar");
        if (song.artist.empty()) song.artist = FieldNameList(o, on, "artists");
        size_t ab = 0, ae = 0;
        if (FieldObject(o, on, "al", &ab, &ae) ||
            FieldObject(o, on, "album", &ab, &ae)) {
            song.album = FieldStr(o + ab, ae - ab, "name");
            song.cover = FieldStr(o + ab, ae - ab, "picUrl");
            if (song.cover.empty()) {
                // Newer search/detail shapes carry a numeric pic id; the
                // cover url is derived with the public cover recipe.
                bool has_pic = false;
                long long pic = FieldNum(o + ab, ae - ab, "pic", &has_pic);
                if (!has_pic) pic = FieldNum(o + ab, ae - ab, "picId",
                                             &has_pic);
                if (has_pic && pic > 0) {
                    char idbuf[32];
                    std::snprintf(idbuf, sizeof(idbuf), "%lld", pic);
                    song.cover = CoverLink(idbuf);
                }
            }
        }
        if (song.cover.empty()) song.cover = FieldStr(o, on, "picUrl");
        bool has_dt = false;
        long long dt = FieldNum(o, on, "dt", &has_dt);
        if (!has_dt) dt = FieldNum(o, on, "duration", &has_dt);
        song.duration_ms = (has_dt && dt > 0) ? dt : 0;
        out.push_back(song);
        ++appended;
        i = obj_end;
    }
    return appended;
}

int ParseLyric(const char *json, size_t len, std::string &lrc_out) {    lrc_out.clear();
    if (!json || len == 0) return -1;
    std::string base, trans;
    for (int pass = 0; pass < 2; ++pass) {
        const char *key = (pass == 0) ? "lrc" : "tlyric";
        size_t q = FindKeyAt(json, len, 0, key, -1);
        if (q == kNpos) continue;
        size_t v = ColonValue(json, len, q, std::strlen(key));
        if (v == kNpos || v >= len) continue;
        if (v < len && json[v] == 'n') continue;  // null
        if (v >= len || json[v] != '{') continue;
        size_t obj_end = MatchBracket(json, len, v);
        if (obj_end == kNpos) continue;
        std::string text =
            FieldStr(json + v, obj_end - v, "lyric");
        if (pass == 0)
            base = text;
        else
            trans = text;
    }
    if (!ValidLrc(base)) return 0;
    if (!ValidLrc(trans)) {
        lrc_out = base;
        return 0;
    }
    // Map translation timestamp -> text.
    struct Pair {
        std::string ts;
        std::string text;
    };
    std::vector<Pair> tmap;
    {
        size_t pos = 0;
        while (pos < trans.size()) {
            size_t eol = trans.find('\n', pos);
            std::string line = trans.substr(
                pos, eol == std::string::npos ? std::string::npos : eol - pos);
            if (!line.empty() && line[0] == '[') {
                size_t rb = line.find(']');
                if (rb != std::string::npos && rb + 1 < line.size()) {
                    std::string text = line.substr(rb + 1);
                    while (!text.empty() &&
                           (text.back() == '\r' || text.back() == ' ' ||
                            text.back() == '\t'))
                        text.pop_back();
                    if (!text.empty())
                        tmap.push_back({line.substr(0, rb + 1), text});
                }
            }
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    }
    std::string merged;
    merged.reserve(base.size() + trans.size() / 2);
    {
        size_t pos = 0;
        while (pos < base.size()) {
            size_t eol = base.find('\n', pos);
            std::string line = base.substr(
                pos, eol == std::string::npos ? std::string::npos : eol - pos);
            merged += line;
            merged += '\n';
            if (!line.empty() && line[0] == '[') {
                size_t rb = line.find(']');
                if (rb != std::string::npos) {
                    std::string ts = line.substr(0, rb + 1);
                    for (const Pair &p : tmap) {
                        if (p.ts == ts) {
                            merged += ts;
                            merged += p.text;
                            merged += '\n';
                            break;
                        }
                    }
                }
            }
            if (merged.size() > kLyricCap) break;
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    }
    lrc_out = merged;
    return 0;
}

std::string ToHttps126(const std::string &url) {
    const char *kHttp = "http://";
    if (url.compare(0, 7, kHttp) != 0) return url;
    size_t host_end = url.find('/', 7);
    std::string host =
        url.substr(7, host_end == std::string::npos ? std::string::npos
                                                   : host_end - 7);
    const char *kSuffix = ".music.126.net";
    if (host.size() >= std::strlen(kSuffix) &&
        host.compare(host.size() - std::strlen(kSuffix), std::strlen(kSuffix),
                     kSuffix) == 0) {
        return "https://" + url.substr(7);
    }
    return url;
}

std::string UrlEncode(const std::string &in) {
    static const char *kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

bool IsDigits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string ParsePlayUrl(const char *json, size_t len,
                         const std::string &expected_id) {
    if (!json || len == 0 || expected_id.empty()) return std::string();
    size_t q = FindKeyAt(json, len, 0, "data", -1);
    if (q == kNpos) return std::string();
    size_t v = ColonValue(json, len, q, 4);
    if (v == kNpos || v >= len || json[v] != '[') return std::string();
    size_t arr_end = MatchBracket(json, len, v);
    if (arr_end == kNpos) return std::string();
    size_t i = v + 1;
    while (i < arr_end) {
        i = SkipWs(json, len, i);
        if (i >= arr_end) break;
        if (json[i] != '{') {
            ++i;
            continue;
        }
        size_t obj_end = MatchBracket(json, len, i);
        if (obj_end == kNpos || obj_end > arr_end) break;
        const char *o = json + i;
        size_t on = obj_end - i;
        bool has_id = false, has_code = false;
        long long id = FieldNum(o, on, "id", &has_id);
        long long code = FieldNum(o, on, "code", &has_code);
        char idbuf[32];
        std::snprintf(idbuf, sizeof(idbuf), "%lld", id);
        bool id_ok = has_id && expected_id == idbuf;
        if (!id_ok || !has_code || code != 200) {
            i = obj_end;
            continue;
        }
        std::string url = FieldStr(o, on, "url");
        if (url.empty()) {
            i = obj_end;
            continue;
        }
        // A trial source carries a freeTrialInfo object; full sources have
        // an explicit null. Anything else is not playable anonymously.
        size_t tq = FindKey(o, on, 0, "freeTrialInfo");
        if (tq == kNpos) {
            i = obj_end;
            continue;
        }
        size_t tv = ColonValue(o, on, tq, 13);
        if (tv == kNpos || tv >= on) {
            i = obj_end;
            continue;
        }
        if (!(on - tv >= 4 && std::memcmp(o + tv, "null", 4) == 0)) {
            i = obj_end;  // trial object present: skip, keep scanning
            continue;
        }
        return url;
    }
    return std::string();
}

}  // namespace netease
