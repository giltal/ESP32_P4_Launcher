/* PAPP port shim: byte order helpers (RISC-V is little-endian) */
#pragma once
#include <stdint.h>
#ifndef htons
#define htons(x) ((uint16_t)((((uint16_t)(x)) >> 8) | (((uint16_t)(x)) << 8)))
#define ntohs(x) htons(x)
#define htonl(x) __builtin_bswap32((uint32_t)(x))
#define ntohl(x) htonl(x)
#endif
