#pragma once
// Minimal HTTPS via JNI (HttpURLConnection) for the plugin workers.
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
// A Cookie header is sent when cookie is non-null.
int HttpGet(JavaVM *vm, const char *url, const char *cookie, int connect_ms,
            int read_ms, size_t max_body, bool header_only,
            HttpResponse *out);

// Form POST (content type application/x-www-form-urlencoded). Redirects are
// followed as GET without a body, like browsers do for 301/302/303.
// Always reads the body. Returns 0 on HTTP 200, negative on failure.
int HttpPost(JavaVM *vm, const char *url, const char *body,
             const char *cookie, int connect_ms, int read_ms,
             size_t max_body, HttpResponse *out);

}  // namespace netease
