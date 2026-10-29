#include "lx_common.h"

static uint32_t s_table[256];

static void crc32_init_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_table[i] = c;
    }
}

uint32_t lx_crc32(uint32_t crc, const void *data, size_t len)
{
    if (s_table[1] == 0) crc32_init_table();
    const uint8_t *p = data;
    crc = ~crc;
    while (len--)
        crc = s_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
