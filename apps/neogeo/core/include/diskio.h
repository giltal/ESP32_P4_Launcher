/*
 * The FatFs subset gngeo's sprite streaming uses, on stdio. The reference
 * reads the .ctile cache with raw disk_read() on LBA sectors; here the
 * "sector map" holds file sectors (bank * sectors_per_bank) and disk_read()
 * reads them from the open .ctile file (gn_ctile_open in roms.c).
 */
#pragma once

#include <stdint.h>

typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint32_t LBA_t;
typedef unsigned UINT;

typedef enum { RES_OK = 0, RES_ERROR, RES_WRPRT, RES_NOTRDY, RES_PARERR } DRESULT;

DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count);
