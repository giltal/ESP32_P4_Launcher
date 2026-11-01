/* PAPP port shim: ESP_LOGx -> launcher log (errors and warnings only by default) */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void odroid_papp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#ifdef __cplusplus
}
#endif
#define ESP_LOGE(tag, fmt, ...) odroid_papp_log('E', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) odroid_papp_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) odroid_papp_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) do { } while (0)
#define ESP_LOGV(tag, fmt, ...) do { } while (0)
#define ESP_EARLY_LOGE ESP_LOGE
#define ESP_EARLY_LOGW ESP_LOGW
#define ESP_EARLY_LOGI ESP_LOGI
