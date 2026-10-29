/* PAPP port of nofrendo: ESP-IDF placement attributes have no meaning in an
 * app. Hot code is placed with PAPP_FAST in the sources instead. */
#pragma once

#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))

/* Hot paths placed in internal SRAM (the PAPP_FAST section; papp.h can't be
 * included next to nofrendo's own `bool`) */
#define NOF_FAST __attribute__((section(".fast.text"), noinline))
