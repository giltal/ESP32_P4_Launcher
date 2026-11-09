/*
 * http_test — HTTP interface test (PAPP_MF_WIFI; http_test_noflag is the same app without the flag).
 *   args: [url] [timeout_ms] [count] [method] [body]
 *   Prints the Wi-Fi state, then sends the request count times (default: GET Open-Meteo, Munich)
 *   and logs status, size, time and the start of the answer. A body is sent as JSON.
 *   Exit code: the last status (negative = error).
 */
#include "papp/papp.h"
#include <stdlib.h>
#include <string.h>

#ifdef HTTP_TEST_NOFLAG
PAPP_MANIFEST(.id = "test.http_noflag", .name = "HTTP test (no flag)", .version = PAPP_APP_VERSION,
              .main_stack = 16384, .flags = PAPP_MF_HIDDEN);
#else
PAPP_MANIFEST(.id = "test.http", .name = "HTTP test", .version = PAPP_APP_VERSION,
              .main_stack = 16384, .flags = PAPP_MF_HIDDEN | PAPP_MF_WIFI);
#endif

#define DEFAULT_URL "https://api.open-meteo.com/v1/forecast?latitude=48.1374&longitude=11.5755" \
                    "&current=temperature_2m,weather_code&timezone=auto"

static const char *state_name(int s)
{
    return s == PAPP_HTTP_CONNECTED ? "connected" : s == PAPP_HTTP_CONNECTING ? "connecting" : "off";
}

int main(int argc, char **argv)
{
    const char *body = argc > 5 ? argv[5] : NULL;
    papp_http_req_t req = {
        .size = sizeof(req),
        .url = argc > 1 ? argv[1] : DEFAULT_URL,
        .timeout_ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 20000,
        .method = argc > 4 ? argv[4] : NULL,
        .headers = body ? "Content-Type: application/json\r\nX-Test: http_test\r\n" : NULL,
        .body = body,
        .body_len = body ? strlen(body) : 0,
    };
    int count = argc > 3 ? atoi(argv[3]) : 1;

    papp_log("http_test: HTTP interface %s, state %s\n", papp_if_http() ? "present" : "missing",
             state_name(papp_http_state()));
    size_t max = 64 * 1024;
    char *buf = papp_alloc(max, PAPP_MEM_ANY, 4);
    if (!buf) return -100;

    int status = 0;
    for (int i = 0; i < count && !papp_exit_requested(); i++) {
        size_t len = 0;
        int64_t t0 = papp_time_us();
        status = papp_http_request(&req, buf, max, &len);
        int ms = (int)((papp_time_us() - t0) / 1000);
        if (len > 300) buf[300] = 0;
        for (char *c = buf; *c; c++)
            if (*c == '\n' || *c == '\r') *c = ' ';
        papp_log("http_test: #%d %s status %d, %u bytes, %d ms: %s\n", i + 1, req.method ? req.method : "GET",
                 status, (unsigned)len, ms, buf);
    }
    papp_free(buf);
    return status;
}
