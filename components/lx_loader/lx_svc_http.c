/*
 * HTTP service: HTTP(S) requests for apps that declare PAPP_MF_WIFI (home.c
 * keeps Wi-Fi running for them only).
 *
 * The request runs on a launcher task with an internal stack (TLS; no network
 * code on an app's PSRAM stack). The app thread waits in short slices, so a
 * stop request or its timeout ends the wait: the request is then abandoned and
 * the worker finishes it on its own. The worker works on launcher copies of the
 * request and reads into a launcher buffer; the app thread copies the answer
 * out, so an abandoned request never touches app memory that teardown may
 * already have freed. No lock is held across the wait: a forced stop can never
 * leave one taken.
 */
#include "lx_services.h"

#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lx_net.h"

static const char *TAG = "lx_svc_http";

#define MAX_ANSWER    (512 * 1024)
#define MAX_BODY      (64 * 1024)       /* request body */
#define MAX_HEADERS   4096
#define MAX_URL       2048
#define WORKER_STACK  10240
#define SLICE_MS      100
#define IO_TIMEOUT_MS 10000     /* per connect / read inside the worker */

enum { JOB_IDLE, JOB_BUSY, JOB_DONE, JOB_ABANDONED };

static struct {
    volatile int state;
    esp_http_client_method_t method;
    char *url, *headers, *body, *answer;    /* launcher heap, owned by the job */
    size_t body_len, max, len;
    int result;
} s_job;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_go, s_done;
static volatile bool s_worker_up;

static const struct { const char *name; esp_http_client_method_t method; } s_methods[] = {
    { "GET", HTTP_METHOD_GET },   { "POST", HTTP_METHOD_POST },     { "PUT", HTTP_METHOD_PUT },
    { "PATCH", HTTP_METHOD_PATCH }, { "DELETE", HTTP_METHOD_DELETE }, { "HEAD", HTTP_METHOD_HEAD },
};

static const char *method_name(esp_http_client_method_t m)
{
    for (size_t i = 0; i < sizeof(s_methods) / sizeof(s_methods[0]); i++)
        if (s_methods[i].method == m) return s_methods[i].name;
    return "?";
}

static void job_free(void)
{
    heap_caps_free(s_job.url);
    heap_caps_free(s_job.headers);
    heap_caps_free(s_job.body);
    heap_caps_free(s_job.answer);
    s_job.url = s_job.headers = s_job.body = s_job.answer = NULL;
}

/* "Name: value" lines (CRLF or LF) from the job's own copy, cut in place */
static void set_headers(esp_http_client_handle_t c, char *h)
{
    while (h && *h) {
        char *end = strchr(h, '\n');
        if (end) *end = 0;
        size_t n = strlen(h);
        if (n && h[n - 1] == '\r') h[n - 1] = 0;
        char *colon = strchr(h, ':');
        if (colon && colon != h) {
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ' || *v == '\t') v++;
            esp_http_client_set_header(c, h, v);
        }
        h = end ? end + 1 : NULL;
    }
}

static int send_body(esp_http_client_handle_t c, const char *body, size_t len)
{
    for (size_t sent = 0; sent < len; ) {
        int n = esp_http_client_write(c, body + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* The Location of a redirect, kept by the event handler (the client has no getter for it) */
static char s_location[MAX_URL + 1];

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Location"))
        snprintf(s_location, sizeof(s_location), "%s", e->header_value);
    return ESP_OK;
}

/* cur becomes the redirect's target. ESP-IDF's own set_redirection refuses every target of an
 * https:// origin that doesn't start with https://, relative ones included; here a relative
 * target is resolved against the current origin, and only a downgrade to http:// is refused. */
static bool redirect_target(char *cur, size_t size)
{
    const char *loc = s_location;
    if (!strncmp(loc, "https://", 8) || !strncmp(loc, "http://", 7)) {
        if (!strncmp(cur, "https://", 8) && strncmp(loc, "https://", 8)) return false;
        if (strlen(loc) >= size) return false;
        strcpy(cur, loc);
        return true;
    }
    if (loc[0] != '/') return false;                    /* none, or relative to the path */
    char *host = strstr(cur, "://") + 3;
    size_t origin = strcspn(host, "/?#") + (size_t)(host - cur);
    if (origin + strlen(loc) >= size) return false;
    strcpy(cur + origin, loc);
    return true;
}

static esp_http_client_handle_t new_client(const char *url, esp_http_client_method_t method)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .event_handler = on_http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .timeout_ms = IO_TIMEOUT_MS,
        .user_agent = "P4-Launcher",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c && s_job.headers) {
        static char h[MAX_HEADERS + 1];            /* set_headers cuts its copy */
        strcpy(h, s_job.headers);
        set_headers(c, h);
    }
    return c;
}

static int fetch(char *answer, size_t max, size_t *len)
{
    *len = 0;
    answer[0] = 0;
    static char cur[MAX_URL + 1];
    strcpy(cur, s_job.url);
    esp_http_client_method_t method = s_job.method;
    size_t body_len = s_job.body_len;
    esp_http_client_handle_t c = NULL;
    int status = PAPP_HTTP_ERR_FAILED;
    /* A new client per hop: one that is reused for the same host hands the cached start of
     * the redirect's own body to the next answer */
    for (int hops = 0; hops < 5; hops++) {
        s_location[0] = 0;
        c = new_client(cur, method);
        if (!c) return PAPP_HTTP_ERR_FAILED;
        if (esp_http_client_open(c, body_len) != ESP_OK || send_body(c, s_job.body, body_len) ||
            esp_http_client_fetch_headers(c) < 0) {
            status = PAPP_HTTP_ERR_FAILED;
            break;
        }
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400 || !redirect_target(cur, sizeof(cur)))
            break;                              /* not followed: the app gets the 3xx */
        if (status == 303) {                    /* "see other": fetch it with GET */
            method = HTTP_METHOD_GET;
            body_len = 0;
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        c = NULL;
        status = PAPP_HTTP_ERR_FAILED;          /* stays so if the redirects never end */
    }
    if (!c) return status;

    size_t got = 0;
    if (status > 0 && s_job.method != HTTP_METHOD_HEAD) {    /* HEAD: headers only, no body to wait for */
        int n;
        while (got < max - 1 && s_job.state != JOB_ABANDONED &&
               (n = esp_http_client_read(c, answer + got, max - 1 - got)) > 0)
            got += n;
        char extra;
        if (got == max - 1 && esp_http_client_read(c, &extra, 1) > 0) status = PAPP_HTTP_ERR_TOO_BIG;
    }
    answer[got] = 0;
    *len = got;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static void worker(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        size_t len;
        int r = fetch(s_job.answer, s_job.max, &len);
        if (r < 0) ESP_LOGW(TAG, "%s %s: error %d", method_name(s_job.method), s_job.url, r);
        bool done;
        taskENTER_CRITICAL(&s_mux);
        done = s_job.state == JOB_BUSY;
        if (done) {
            s_job.result = r;
            s_job.len = len;
            s_job.state = JOB_DONE;
        }
        taskEXIT_CRITICAL(&s_mux);
        if (done) {
            xSemaphoreGive(s_done);
        } else {                            /* the app gave up on it */
            job_free();
            s_job.state = JOB_IDLE;
        }
    }
}

void lx_svc_http_init(void)
{
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
}

/* The worker comes with the first request (callers are serialized by the job claim) */
static bool worker_start(void)
{
    if (s_worker_up) return true;
    if (!s_go || !s_done || xTaskCreate(worker, "lx_svc_http", WORKER_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the worker");
        return false;
    }
    s_worker_up = true;
    return true;
}

static int http_state_impl(void)
{
    if (!(lx_app_flags() & PAPP_MF_WIFI)) return PAPP_HTTP_OFF;
    switch (lx_net_state()) {
    case LX_NET_CONNECTED:
        return PAPP_HTTP_CONNECTED;
    case LX_NET_IDLE:
    case LX_NET_SCANNING:
    case LX_NET_CONNECTING:
        return lx_net_ssid()[0] ? PAPP_HTTP_CONNECTING : PAPP_HTTP_OFF;
    default:
        return PAPP_HTTP_OFF;
    }
}

static int http_state(void)
{
    SVC_ENTER();
    int r = http_state_impl();
    SVC_LEAVE();
    return r;
}

static char *dup_psram(const void *src, size_t n)
{
    char *p = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
    if (p) {
        memcpy(p, src, n);
        p[n] = 0;
    }
    return p;
}

static int request_impl(const papp_http_req_t *app_req, char *buf, size_t max, size_t *len)
{
    if (len) *len = 0;
    if (!app_req || app_req->size < sizeof(papp_http_req_t) || !buf || max < 2) return PAPP_HTTP_ERR_ARG;
    papp_http_req_t req = *app_req;     /* v1 fields; later versions append */
    const char *url = req.url;
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || strlen(url) > MAX_URL)
        return PAPP_HTTP_ERR_ARG;
    esp_http_client_method_t method = HTTP_METHOD_MAX;
    for (size_t i = 0; i < sizeof(s_methods) / sizeof(s_methods[0]); i++)
        if (!req.method || !strcmp(req.method, s_methods[i].name)) {
            method = s_methods[i].method;
            break;
        }
    if (method == HTTP_METHOD_MAX || req.body_len > MAX_BODY || (req.body_len && !req.body) ||
        (req.headers && strlen(req.headers) > MAX_HEADERS))
        return PAPP_HTTP_ERR_ARG;
    buf[0] = 0;
    if (max > MAX_ANSWER) max = MAX_ANSWER;
    int64_t deadline = esp_timer_get_time() + (int64_t)req.timeout_ms * 1000;

    for (int st; (st = http_state_impl()) != PAPP_HTTP_CONNECTED; vTaskDelay(pdMS_TO_TICKS(SLICE_MS))) {
        if (lx_svc_exit_flag) return PAPP_HTTP_ERR_EXIT;
        if (st == PAPP_HTTP_OFF || esp_timer_get_time() > deadline) return PAPP_HTTP_ERR_OFFLINE;
    }
    for (bool claimed = false; !claimed; ) {              /* IDLE -> BUSY: ours */
        taskENTER_CRITICAL(&s_mux);
        if (s_job.state == JOB_IDLE) { s_job.state = JOB_BUSY; claimed = true; }
        taskEXIT_CRITICAL(&s_mux);
        if (claimed) break;
        if (lx_svc_exit_flag) return PAPP_HTTP_ERR_EXIT;
        if (esp_timer_get_time() > deadline) return PAPP_HTTP_ERR_BUSY;
        vTaskDelay(pdMS_TO_TICKS(SLICE_MS));
    }

    if (!worker_start()) {
        s_job.state = JOB_IDLE;
        return PAPP_HTTP_ERR_FAILED;
    }
    s_job.method = method;
    s_job.url = dup_psram(url, strlen(url));
    s_job.headers = req.headers ? dup_psram(req.headers, strlen(req.headers)) : NULL;
    s_job.body = req.body_len ? dup_psram(req.body, req.body_len) : NULL;
    s_job.body_len = req.body_len;
    s_job.answer = heap_caps_malloc(max, MALLOC_CAP_SPIRAM);
    if (!s_job.url || (req.headers && !s_job.headers) || (req.body_len && !s_job.body) || !s_job.answer) {
        job_free();
        s_job.state = JOB_IDLE;
        return PAPP_HTTP_ERR_FAILED;
    }
    s_job.max = max;
    xSemaphoreTake(s_done, 0);
    xSemaphoreGive(s_go);

    while (xSemaphoreTake(s_done, pdMS_TO_TICKS(SLICE_MS)) != pdTRUE) {
        bool stop = lx_svc_exit_flag;
        if (!stop && esp_timer_get_time() <= deadline) continue;
        bool finished;
        taskENTER_CRITICAL(&s_mux);
        finished = s_job.state == JOB_DONE;
        if (!finished) s_job.state = JOB_ABANDONED;     /* the worker frees it */
        taskEXIT_CRITICAL(&s_mux);
        if (finished) break;                            /* just made it */
        return stop ? PAPP_HTTP_ERR_EXIT : PAPP_HTTP_ERR_TIMEOUT;
    }

    int r = s_job.result;
    memcpy(buf, s_job.answer, s_job.len + 1);
    if (len) *len = s_job.len;
    job_free();
    s_job.state = JOB_IDLE;
    return r;
}

static int http_request(const papp_http_req_t *req, char *buf, size_t max, size_t *len)
{
    SVC_ENTER();
    int r = request_impl(req, buf, max, len);
    SVC_LEAVE();
    return r;
}

const papp_if_http_t lx_if_http = {
    .hdr = { PAPP_IF_HTTP, PAPP_IF_HTTP_VER, sizeof(papp_if_http_t) },
    .state = http_state,
    .request = http_request,
};
