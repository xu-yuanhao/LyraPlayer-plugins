// Host unit test for netease_json.cpp. Uses real captured payloads.
// Build: g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I. \
//   netease_json.cpp crypto.cpp netease_json_test.cpp -o /tmp/netease_json_test
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "netease_json.h"

namespace {

std::string ReadFile(const char *path) {
    FILE *f = std::fopen(path, "rb");
    assert(f);
    std::string out;
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::fflush(stderr);
        assert(cond);
    }
    std::printf("ok: %s\n", msg);
}

}  // namespace

int main(int argc, char **argv) {
    assert(argc == 2);
    std::string dir = argv[1];

    // 1. search/get: only fee==0 songs survive, metadata intact.
    {
        std::string body = ReadFile((dir + "/search.json").c_str());
        std::vector<netease::SongInfo> songs;
        int n = netease::ParseSongs(body.data(), body.size(), songs, 10);
        Expect(n == static_cast<int>(songs.size()), "search returns count");
        Expect(n > 0 && n <= 10, "search yields some free songs");
        for (const auto &s : songs) {
            Expect(s.fee == 0, "no VIP/paid song published");
            Expect(!s.id.empty() && netease::IsDigits(s.id), "numeric id");
            Expect(!s.title.empty(), "title present");
            Expect(!s.artist.empty(), "artist present");
            Expect(s.duration_ms > 0, "duration from search/get");
        }
        bool has_cover = false;
        for (const auto &s : songs)
            if (!s.cover.empty()) has_cover = true;
        Expect(has_cover, "picId cover derived");
        for (const auto &s : songs)
            if (!s.cover.empty())
                Expect(s.cover.compare(0, 7, "http://") == 0 &&
                           s.cover.find(".music.126.net") !=
                               std::string::npos,
                       "cover is a 126 url");
        std::printf("  first: %s / %s [%s]\n", songs[0].title.c_str(),
                    songs[0].artist.c_str(), songs[0].id.c_str());
    }

    // 2. v3 song/detail shares the same song object shape ("artists").
    {
        std::string body = ReadFile((dir + "/detail.json").c_str());
        std::vector<netease::SongInfo> songs;
        int n = netease::ParseSongs(body.data(), body.size(), songs, 10);
        Expect(n == 1, "detail yields one song");
        Expect(songs[0].id == "3362265838", "detail id");
        Expect(songs[0].artist == "潮汕家丁", "detail artist");
        Expect(!songs[0].cover.empty(), "detail cover");
    }

    // 2b. player/url/v1: full source accepted, VIP/trial rejected.
    {
        std::string free_body = ReadFile((dir + "/url_free.json").c_str());
        std::string url = netease::ParsePlayUrl(
            free_body.data(), free_body.size(), "3362265838");
        Expect(!url.empty(), "free track has playable url");
        Expect(url.find("music.126.net") != std::string::npos,
               "play url points at 126");
        Expect(netease::ParsePlayUrl(free_body.data(), free_body.size(),
                                     "99999")
                   .empty(),
               "wrong id rejected");
        std::string vip_body = ReadFile((dir + "/url_vip.json").c_str());
        Expect(netease::ParsePlayUrl(vip_body.data(), vip_body.size(),
                                     "1357375695")
                   .empty(),
               "VIP track has no playable url");
        Expect(netease::ParsePlayUrl("{}", 2, "1").empty(),
               "empty url payload rejected");
    }

    // 3. lyric: base + translation merged, valid LRC.
    {
        std::string body = ReadFile((dir + "/lyric.json").c_str());
        std::string lrc;
        int rc = netease::ParseLyric(body.data(), body.size(), lrc);
        Expect(rc == 0, "lyric parses");
        Expect(!lrc.empty(), "lyric non-empty");
        Expect(lrc.find('[') != std::string::npos, "lyric has timestamps");
        Expect(lrc.find("寒夜里看雪飘过") != std::string::npos,
               "lyric keeps base line");
        Expect(lrc.size() < (1u << 20), "lyric bounded");
    }

    // 4. malformed inputs are rejected, never crash.
    {
        std::vector<netease::SongInfo> songs;
        Expect(netease::ParseSongs("", 0, songs, 10) == -1, "empty rejected");
        Expect(netease::ParseSongs("{}", 2, songs, 10) == -1,
               "no songs rejected");
        Expect(netease::ParseSongs("{\"songs\":[", 10, songs, 10) == -1,
               "truncated rejected");
        std::string lrc;
        Expect(netease::ParseLyric("", 0, lrc) == -1, "empty lyric rejected");
        Expect(netease::ParseLyric("{}", 2, lrc) == 0 && lrc.empty(),
               "no lyric block tolerated");
        Expect(!netease::IsDigits(""), "empty not digits");
        Expect(netease::IsDigits("123"), "digits ok");
        Expect(!netease::IsDigits("12a"), "alpha rejected");
    }

    // 5. helpers.
    {
        Expect(netease::ToHttps126("http://m701.music.126.net/x.mp3") ==
                   "https://m701.music.126.net/x.mp3",
               "126 http upgraded");
        Expect(netease::ToHttps126("https://m701.music.126.net/x.mp3") ==
                   "https://m701.music.126.net/x.mp3",
               "https untouched");
        Expect(netease::ToHttps126("https://example.com/x.mp3") ==
                   "https://example.com/x.mp3",
               "foreign host untouched");
        Expect(netease::UrlEncode("海阔天空 Beyond") ==
                   "%E6%B5%B7%E9%98%94%E5%A4%A9%E7%A9%BA%20Beyond",
               "query encoded");
    }

    std::printf("ALL PASS\n");
    return 0;
}
