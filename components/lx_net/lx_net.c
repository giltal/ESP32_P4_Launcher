#include "lx_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "lx_net";

#define NS            "net"
#define MAX_RETRIES   4
#define TASK_STACK    6144

typedef enum { CMD_SCAN, CMD_CONNECT, CMD_FORGET, CMD_PAUSE, CMD_RESUME, CMD_RETRY } cmd_t;

static const struct { const char *name, *posix; } k_zones[] = {
    { "Not set", NULL },
    { "UTC", "UTC0" },
    { "London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Israel", "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "India", "IST-5:30" },
    { "China", "CST-8" },
    { "Japan", "JST-9" },
    { "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "New York", "EST5EDT,M3.2.0,M11.1.0" },
    { "Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },
};
#define ZONES ((int)(sizeof(k_zones) / sizeof(k_zones[0])))

static QueueHandle_t s_q;
static volatile lx_net_state_t s_state = LX_NET_OFF;
static bool s_started, s_paused, s_want, s_save, s_sntp;
static int s_retries, s_tz;
static char s_ssid[33], s_pass[65], s_ip[16];
static lx_net_ap_t s_aps[LX_NET_MAX_APS];
static volatile int s_ap_count = -1;

static void post(cmd_t c)
{
    if (s_q) xQueueSend(s_q, &c, 0);
}

/* ── Saved network ───────────────────────────────────────────── */

static void creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t a = sizeof(s_ssid), b = sizeof(s_pass);
    if (nvs_get_str(h, "ssid", s_ssid, &a) != ESP_OK) s_ssid[0] = 0;
    if (nvs_get_str(h, "pass", s_pass, &b) != ESP_OK) s_pass[0] = 0;
    uint8_t tz;
    if (nvs_get_u8(h, "tz", &tz) == ESP_OK && tz < ZONES) s_tz = tz;
    nvs_close(h);
}

static void creds_save(bool keep)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (keep) {
        nvs_set_str(h, "ssid", s_ssid);
        nvs_set_str(h, "pass", s_pass);
    } else {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ── Events ──────────────────────────────────────────────────── */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = 0;
        if (s_paused || !s_want) return;
        if (++s_retries > MAX_RETRIES) {
            s_want = false;
            s_state = LX_NET_FAILED;
            ESP_LOGW(TAG, "could not join %s", s_ssid);
            return;
        }
        s_state = LX_NET_CONNECTING;
        post(CMD_RETRY);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_state = LX_NET_CONNECTED;
        ESP_LOGI(TAG, "connected to %s, %s", s_ssid, s_ip);
        if (s_save) {
            s_save = false;
            creds_save(true);
        }
        if (!s_sntp) {
            s_sntp = true;
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
        }
    }
}

static bool radio_start(void)
{
    if (s_started) return true;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e == ESP_OK) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        e = esp_wifi_start();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "radio start failed: %s", esp_err_to_name(e));
        s_state = LX_NET_NO_RADIO;
        return false;
    }
    s_started = true;
    if (s_state == LX_NET_OFF) s_state = LX_NET_IDLE;
    return true;
}

static void join(void)
{
    wifi_config_t c = { 0 };
    memcpy(c.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(c.sta.ssid)));   /* may fill the field */
    memcpy(c.sta.password, s_pass, strnlen(s_pass, sizeof(c.sta.password)));
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
}

static void do_scan(void)
{
    s_ap_count = -1;
    if (!radio_start()) {
        s_ap_count = 0;
        return;
    }
    lx_net_state_t was = s_state;
    s_state = LX_NET_SCANNING;
    uint16_t found = 0, n = LX_NET_MAX_APS;
    wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
    if (rec && esp_wifi_scan_start(NULL, true) == ESP_OK) {
        esp_wifi_scan_get_ap_num(&found);
        esp_wifi_scan_get_ap_records(&n, rec);
    } else {
        n = 0;
    }
    int out = 0;
    for (int i = 0; i < n; i++) {           /* strongest first; one line per name */
        bool dup = !rec[i].ssid[0];
        for (int j = 0; j < out; j++) dup |= strcmp(s_aps[j].ssid, (char *)rec[i].ssid) == 0;
        if (dup) continue;
        snprintf(s_aps[out].ssid, sizeof(s_aps[out].ssid), "%s", (char *)rec[i].ssid);
        s_aps[out].rssi = rec[i].rssi;
        s_aps[out].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        out++;
    }
    free(rec);
    s_state = s_state == LX_NET_SCANNING ? was : s_state;
    s_ap_count = out;
}

static void net_task(void *arg)
{
    if (s_ssid[0]) {
        s_want = true;
        s_state = LX_NET_CONNECTING;
        if (radio_start()) join();
    }
    for (;;) {
        cmd_t c;
        if (!xQueueReceive(s_q, &c, portMAX_DELAY)) continue;
        switch (c) {
        case CMD_SCAN:
            do_scan();
            break;
        case CMD_CONNECT:
            s_want = true;
            s_retries = 0;
            s_state = LX_NET_CONNECTING;
            if (s_started && !s_paused) esp_wifi_disconnect();
            if (radio_start()) join();
            break;
        case CMD_RETRY:
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (s_want && !s_paused) join();
            break;
        case CMD_FORGET:
            s_want = false;
            s_ssid[0] = s_pass[0] = 0;
            creds_save(false);
            if (s_started) esp_wifi_disconnect();
            s_state = s_started ? LX_NET_IDLE : LX_NET_OFF;
            break;
        case CMD_PAUSE:
            if (s_started && !s_paused) {
                s_paused = true;
                esp_wifi_stop();
            }
            break;
        case CMD_RESUME:
            if (s_paused) {
                s_paused = false;
                esp_wifi_start();
                if (s_want) {
                    s_retries = 0;
                    s_state = LX_NET_CONNECTING;
                    join();
                } else if (s_state != LX_NET_FAILED) {
                    s_state = LX_NET_IDLE;
                }
            }
            break;
        }
    }
}

/* ── Public ──────────────────────────────────────────────────── */

esp_err_t lx_net_init(void)
{
    creds_load();
    if (k_zones[s_tz].posix) {
        setenv("TZ", k_zones[s_tz].posix, 1);
        tzset();
    }
    s_q = xQueueCreate(8, sizeof(cmd_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(net_task, "lx_net", TASK_STACK, NULL, 4, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

lx_net_state_t lx_net_state(void) { return s_state; }
const char *lx_net_ssid(void)     { return s_ssid; }
const char *lx_net_ip(void)       { return s_ip; }

void lx_net_scan(void)
{
    s_ap_count = -1;
    post(CMD_SCAN);
}

int lx_net_scan_results(lx_net_ap_t *out, int max)
{
    int n = s_ap_count;
    if (n < 0) return -1;
    if (n > max) n = max;
    memcpy(out, s_aps, (size_t)n * sizeof(*out));
    return n;
}

void lx_net_connect(const char *ssid, const char *password)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    s_save = true;
    post(CMD_CONNECT);
}

void lx_net_forget(void)      { post(CMD_FORGET); }
void lx_net_pause(bool pause) { post(pause ? CMD_PAUSE : CMD_RESUME); }

int         lx_net_tz_count(void)      { return ZONES; }
const char *lx_net_tz_name(int index)  { return index >= 0 && index < ZONES ? k_zones[index].name : ""; }
int         lx_net_tz_get(void)        { return s_tz; }

void lx_net_tz_set(int index)
{
    if (index < 0 || index >= ZONES) return;
    s_tz = index;
    if (k_zones[index].posix) setenv("TZ", k_zones[index].posix, 1);
    else unsetenv("TZ");
    tzset();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "tz", (uint8_t)index);
        nvs_commit(h);
        nvs_close(h);
    }
}
