/* Stub for ESP-IDF attribute macros — PSRAM app build */
#ifndef PAPP_COMPAT_ESP_ATTR_H
#define PAPP_COMPAT_ESP_ATTR_H

#ifndef IRAM_ATTR            /* the PAPP build defines it as the FAST section */
#define IRAM_ATTR
#endif
#define DRAM_ATTR
#define WORD_ALIGNED_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR

#endif
