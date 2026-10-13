#pragma once
// Minimal HTTPS GET over JNI (HttpURLConnection) for the plugin workers.
// Only needs a JavaVM; no Android Context is required. No exceptions.
#include <cstddef>
#include <string>

typedef struct _JavaVM JavaVM;

namespace netease {

struct HttpResponse {
    int code = -1;            // last HTTP status, -1 on local failure
    std::string body;         // response body (empty when header_only)
    std::string final_url;    // last fetched url (200) or redirect target
};

// GET with manual redirect handling (up to 6 hops). Follows the host
// resource limits in spirit: bounded time and bytes. Returns 0 on HTTP 200
// (final_url + body set, unless header_only), 1 when the chain ended at a
// redirect (final_url = last Location, body empty), negative on failure.
int HttpGet(JavaVM *vm, const char *url, int connect_ms, int read_ms,
            size_t max_body, bool header_only, HttpResponse *out);

}  // namespace netease
