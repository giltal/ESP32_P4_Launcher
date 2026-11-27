/*
 * One-time move to the bigger flash layout (launcher/partitions.csv): two 6.5 MB launcher slots instead of
 * a 3 MB factory slot + two 3 MB update slots. Settings -> Update can't change the partition table, so a
 * launcher that finds the old table on the flash rewrites it itself, once, after it was confirmed:
 *
 *   1. running from an update slot: copy this launcher into the factory slot (0x20000 = the new ota_0),
 *      check its hash there;
 *   2. erase the new coredump area (unused space in the old table);
 *   3. erase otadata: the old table now boots the factory slot = this launcher;
 *   4. erase + write the partition table sector, read it back;
 *   5. restart: the new table, no otadata = ota_0 (0x20000) = this launcher.
 *
 * A power cut during 1-3 changes nothing that boots; only one during 4 (well under a second) leaves a board
 * that needs the full image flashed over USB once. Flash writes run on the caller's task: it must have an
 * internal stack (home screen task).
 */
#include "lx_update.h"

#include <stdlib.h>
#include <string.h>
#include "esp_flash.h"
#include "esp_flash_internal.h"   /* esp_flash_set_dangerous_write_protection */
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_ota_ops.h"

static const char *TAG = "lx_layout";

#define TABLE_ADDR   0x8000
#define TABLE_LEN    0xC00          /* what gen_esp32part writes (ESP_PARTITION_TABLE_MAX_LEN) */
#define SLOT_ADDR    0x20000        /* old factory = new ota_0 */
#define OLD_SLOT     0x300000

typedef struct {
    const char *label;
    uint8_t     type, subtype;
    uint32_t    addr, size;
} part_t;

/* launcher/partitions.csv: keep both in step (tests/host/test_layout.py compares them) */
static const part_t k_new[] = {
    { "nvs",      1, 0x02, 0x9000,   0x6000 },
    { "otadata",  1, 0x00, 0xF000,   0x2000 },
    { "ota_0",    0, 0x10, 0x20000,  0x680000 },
    { "ota_1",    0, 0x11, 0x6A0000, 0x680000 },
    { "coredump", 1, 0x03, 0xD20000, 0x40000 },
};
static const part_t k_old[] = {
    { "nvs",      1, 0x02, 0x9000,   0x6000 },
    { "otadata",  1, 0x00, 0xF000,   0x2000 },
    { "factory",  0, 0x00, 0x20000,  0x300000 },
    { "ota_0",    0, 0x10, 0x320000, 0x300000 },
    { "ota_1",    0, 0x11, 0x620000, 0x300000 },
    { "coredump", 1, 0x03, 0x920000, 0x40000 },
};
#define N(a) (sizeof(a) / sizeof((a)[0]))

/* The table as gen_esp32part.py builds it: 32-byte entries, an MD5 entry, 0xFF up to 0xC00 */
static void build_table(uint8_t *t)
{
    memset(t, 0xFF, TABLE_LEN);
    uint8_t *e = t;
    for (size_t i = 0; i < N(k_new); i++, e += 32) {
        memset(e, 0, 32);
        e[0] = 0xAA;
        e[1] = 0x50;
        e[2] = k_new[i].type;
        e[3] = k_new[i].subtype;
        memcpy(e + 4, &k_new[i].addr, 4);       /* little endian, as the chip */
        memcpy(e + 8, &k_new[i].size, 4);
        strncpy((char *)e + 12, k_new[i].label, 16);
    }
    md5_context_t c;
    esp_rom_md5_init(&c);
    esp_rom_md5_update(&c, t, (uint32_t)(e - t));
    e[0] = 0xEB;
    e[1] = 0xEB;                                /* 14 bytes 0xFF, then the digest */
    esp_rom_md5_final(e + 16, &c);
}

static bool table_is(const part_t *want, size_t n)
{
    size_t count = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->flash_chip != esp_flash_default_chip) continue;
        size_t i;
        for (i = 0; i < n; i++)
            if (!strcmp(p->label, want[i].label) && p->type == want[i].type && p->subtype == want[i].subtype &&
                p->address == want[i].addr && p->size == want[i].size) break;
        if (i == n) {
            esp_partition_iterator_release(it);
            return false;
        }
        count++;
    }
    return count == n;
}

bool lx_update_layout_old(void)
{
    return table_is(k_old, N(k_old));
}

const char *lx_update_layout_name(void)
{
    if (table_is(k_new, N(k_new))) return "6.5 MB slots";
    if (table_is(k_old, N(k_old))) return "3 MB slots (old)";
    return "other";
}

static esp_err_t copy_running(const esp_partition_t *run, const esp_partition_t *dst, size_t len,
                              void (*progress)(int))
{
    const size_t CH = 0x10000;
    uint8_t *a = heap_caps_malloc(CH, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *b = heap_caps_malloc(CH, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = a && b ? ESP_OK : ESP_ERR_NO_MEM;
    size_t span = (len + 0xFFF) & ~0xFFFu;
    if (err == ESP_OK) err = esp_partition_erase_range(dst, 0, span);
    for (size_t off = 0; err == ESP_OK && off < span; off += CH) {
        size_t n = span - off < CH ? span - off : CH;
        err = esp_partition_read(run, off, a, n);
        if (err == ESP_OK) err = esp_partition_write(dst, off, a, n);
        if (err == ESP_OK) err = esp_partition_read(dst, off, b, n);
        if (err == ESP_OK && memcmp(a, b, n)) err = ESP_ERR_INVALID_CRC;
        if (progress) progress((int)((off + n) * 80 / span));
    }
    free(a);
    free(b);
    return err;
}

esp_err_t lx_update_layout_migrate(void (*progress)(int percent))
{
    if (!lx_update_layout_old()) return ESP_ERR_INVALID_STATE;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
        return ESP_ERR_INVALID_STATE;                       /* not confirmed yet: a rollback may follow */

    esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
    esp_image_metadata_t md;
    esp_err_t err = esp_image_get_metadata(&pos, &md);
    if (err != ESP_OK || md.image_len == 0 || md.image_len > OLD_SLOT) {
        ESP_LOGE(TAG, "running image: %s, %u bytes", esp_err_to_name(err), (unsigned)md.image_len);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGW(TAG, "moving to the bigger flash layout: running from %s (0x%lx), %u bytes", run->label,
             (unsigned long)run->address, (unsigned)md.image_len);

    /* 1. this launcher at 0x20000 */
    if (run->address != SLOT_ADDR) {
        const esp_partition_t *fac = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                              ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (!fac || fac->address != SLOT_ADDR) return ESP_ERR_NOT_FOUND;
        err = copy_running(run, fac, md.image_len, progress);
        if (err == ESP_OK) {
            esp_partition_pos_t fp = { .offset = fac->address, .size = fac->size };
            esp_image_metadata_t fm;
            err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &fp, &fm);     /* checksum + SHA-256 */
            if (err == ESP_OK && fm.image_len != md.image_len) err = ESP_ERR_INVALID_SIZE;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "copy to the factory slot failed: %s (nothing else changed)", esp_err_to_name(err));
            return err;
        }
    }
    if (progress) progress(85);

    /* 2. the new coredump area: free space today, garbage would read as a crash record */
    const part_t *cd = &k_new[N(k_new) - 1];
    err = esp_flash_erase_region(NULL, cd->addr, cd->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump area: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. no otadata: the old table boots the factory slot, the new one ota_0 — both this launcher */
    const esp_partition_t *otad = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!otad) return ESP_ERR_NOT_FOUND;
    err = esp_partition_erase_range(otad, 0, otad->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "otadata: %s", esp_err_to_name(err));
        return err;
    }
    if (progress) progress(90);

    /* 4. the table */
    uint8_t *t = heap_caps_malloc(TABLE_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *r = heap_caps_malloc(TABLE_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!t || !r) {
        free(t);
        free(r);
        return ESP_ERR_NO_MEM;
    }
    build_table(t);
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
    for (int tries = 0; tries < 3; tries++) {
        err = esp_flash_erase_region(NULL, TABLE_ADDR, 0x1000);
        if (err == ESP_OK) err = esp_flash_write(NULL, t, TABLE_ADDR, TABLE_LEN);
        if (err == ESP_OK) err = esp_flash_read(NULL, r, TABLE_ADDR, TABLE_LEN);
        if (err == ESP_OK && memcmp(t, r, TABLE_LEN)) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK) break;
        ESP_LOGE(TAG, "partition table write %d: %s", tries + 1, esp_err_to_name(err));
    }
    esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
    free(t);
    free(r);
    if (err != ESP_OK) return err;         /* the board needs the full image over USB now */
    if (progress) progress(100);
    ESP_LOGW(TAG, "new partition table written, restarting");
    return ESP_OK;                         /* the caller restarts (after showing it) */
}
