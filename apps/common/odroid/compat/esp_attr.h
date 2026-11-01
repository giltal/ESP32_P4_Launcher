/* PAPP port shim: placement attributes. IRAM code goes to the app's FAST
 * region (internal SRAM); data placement attributes are dropped. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define IRAM_ATTR          __attribute__((section(".fast.text")))
#define DRAM_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define NOINIT_ATTR
#define WORD_ALIGNED_ATTR  __attribute__((aligned(4)))
#define DMA_ATTR
#ifndef IRAM_ATTR_NOINLINE
#define IRAM_ATTR_NOINLINE IRAM_ATTR
#endif
