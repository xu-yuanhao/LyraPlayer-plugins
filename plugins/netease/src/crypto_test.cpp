// Host unit test for crypto.cpp.
// Build: g++ -std=c++17 -fno-exceptions -fno-rtti -Wall -Wextra -I. \
//   crypto.cpp crypto_test.cpp -o /tmp/crypto_test
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "crypto.h"

namespace {

void Expect(bool cond, const char *msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        std::fflush(stderr);
        assert(cond);
    }
    std::printf("ok: %s\n", msg);
}

std::string Hex(const uint8_t *d, size_t n) {
    static const char *k = "0123456789abcdef";
    std::string o;
    for (size_t i = 0; i < n; ++i) {
        o.push_back(k[d[i] >> 4]);
        o.push_back(k[d[i] & 15]);
    }
    return o;
}

}  // namespace

int main() {
    using netease::crypto::Aes128Ecb;
    using netease::crypto::Md5Hex;

    Expect(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e", "md5 empty");
    Expect(Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72", "md5 abc");
    Expect(Md5Hex("nobody/api/song/enhance/player/url/v1use{}md5forencrypt")
                   .size() == 32,
           "md5 shape");

    // NIST FIPS-197 Appendix B vector.
    {
        uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                           0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
        uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
        std::vector<uint8_t> enc;
        Expect(Aes128Ecb(key, pt, 16, enc, true), "aes enc ok");
        // PKCS7 on block-aligned input adds a full padding block.
        Expect(enc.size() == 32, "aes pad block");
        Expect(Hex(enc.data(), 16) == "69c4e0d86a7b0430d8cdb78070b4c55a",
               "aes nist vector");
        std::vector<uint8_t> dec;
        Expect(Aes128Ecb(key, enc.data(), enc.size(), dec, false),
               "aes dec ok");
        Expect(dec.size() == 16 &&
                   std::string(reinterpret_cast<char *>(dec.data()), 16) ==
                       std::string(reinterpret_cast<char *>(pt), 16),
               "aes roundtrip");
        std::vector<uint8_t> bad;
        Expect(!Aes128Ecb(key, pt, 15, bad, false), "aes dec rejects odd");
    }

    // eapi envelope cross-checked against openssl (see build notes).
    {
        std::string header = netease::EapiHeaderJson(
            "0123456789abcdef0123456789abcdef", "1234567890123_0042");
        std::string body =
            "{\"ids\":\"[3362265838]\",\"level\":\"standard\","
            "\"encodeType\":\"mp3\",\"header\":" +
            header + ",\"e_r\":false}";
        std::string params = netease::EapiParams(
            "/api/song/enhance/player/url/v1", body);
        Expect(
            params ==
                "FA90B329E9614F79E79598F37DC2EDB487F00D1BC4C9B24CD57E6C318B90735"
                "69338432CD7D98D1A3626E997A2C53121538F9F2AB2CBDA234A0B54CB42A325A"
                "3F84B73DC579C5E1C5B0B45149678B5E325F07A7BDB80E1AF285B6BF435A3C2"
                "DDB7ED5A2605699CDDDDF7EFA5ACC0B1818D4A4D183A93926FD0DA19EBFE7A2"
                "8748F5AA396A0B1CF2D1CAD72E31E4842E61DDA451F2CF223C14208A364B7F0"
                "F5A1A0BF35ED2DA479AF573A24467447D810B4F2DFC0F11332FC3C9059FA671"
                "603BD63BAF685459C374E667B76B94D1E0177E3E5F314F4607E45EC7C883B01"
                "AC16111A8FB3A12B7B750B0E6C11C317829B93486B95F8EDAEAC22C35190679"
                "64B6D3F6C997E67CE2E4C5E7256D5C9B731C1148236FBAC73F862D38F5C9170"
                "0674B56C5AD31F97061A58E550BBA610A5DA31DD1A625D1BFE65ED459898B79"
                "13A2D43E7C3F04EC52FE6C817895DB6B00DE2D175AD5130DDC21191A048BD0B"
                "B916BDD04A46E744909471043A06332A0B7DA74A6C0606D29644250BC3A4095"
                "E0DE1FBBBB999FE1FC5B28A96ECEC62D22B6F7AB6381480795EFC98DB99398A"
                "C83185AE4C5B114D6862932BD9D5A3385C50C42F909323FD61C9F385C72635E"
                "7263ACF09972727F68BF939CF730474CD633029573D79A0622B9F34103C547A"
                "9B28ACB2A61BF55F396F0F4194A86120CA254A7C62EDC7958EF4683F691A8CA"
                "E6C6C5A16F57FB8",
            "eapi matches openssl reference");
    }

    // Cover link matches the real CDN object observed on 2026-10-13.
    {
        std::string link = netease::CoverLink("109951172932057113");
        Expect(link ==
                   "http://p2.music.126.net/0nPwAKi3hz675GCplpp8EQ==/"
                   "109951172932057113.jpg",
               "cover link exact");
        Expect(netease::CoverLink("").empty(), "cover rejects empty");
        Expect(netease::CoverLink("12a").empty(), "cover rejects alpha");
    }

    {
        std::string a = netease::RandomId(32);
        std::string b = netease::RandomId(32);
        Expect(a.size() == 32 && b.size() == 32 && a != b, "random ids");
    }

    std::printf("ALL PASS\n");
    return 0;
}
