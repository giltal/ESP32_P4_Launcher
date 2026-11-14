/*
 * Pico Held 2 - hardware revision switch and pin map.
 *
 * The two revisions differ only in the Start/Select buttons:
 *   v1.3: Start = GPIO35 (active low), Select = GPIO45 (active high)
 *   v1.4: Start = GPIO49 (analog, pressed above BOARD_START_ADC_THRESHOLD), Select = GPIO35 (active low)
 *
 * Select the revision by editing the default below, or on the command line:
 *   idf.py -B build_picoheld2_v13 -DLX_BOARD=picoheld2 -DPICOHELD2_REV=13 build
 */
#pragma once

#ifndef PICOHELD2_REV
#define PICOHELD2_REV 14        /* 13 = Pico Held 2 v1.3, 14 = v1.4 (released) */
#endif
#if PICOHELD2_REV != 13 && PICOHELD2_REV != 14
#error "PICOHELD2_REV must be 13 or 14"
#endif

#if PICOHELD2_REV == 13
#define BOARD_NAME "Pico Held 2 v1.3 (720x720)"  // non-public prototype
#else
#define BOARD_NAME "Pico Held 2 v1.4 (720x720)"
#endif

/* ── Power ─────────────────────────────────────────────────────────────── */
#define BOARD_PWR_EN        52      /* held high = stay on after the ON key is released */

/* ── Display: HD395003C30 720x720, MIPI-DSI 2 lanes ────────────────────── */
#define BOARD_LCD_H_RES     720
#define BOARD_LCD_V_RES     720
#define BOARD_LCD_BL_EN     7       /* active high */
#define BOARD_LCD_BL_PWM    22
#define BOARD_LCD_BL_FREQ   20000
#define BOARD_LCD_RST       23      /* IOVCC is 1.8 V: never drive this pin high, release it */
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_MV   2500

/* The panel is mounted rotated 90 deg clockwise (bsp.h display_rot). Use 270 if the picture is upside down. */
#ifndef PICOHELD2_LCD_ROT
#define PICOHELD2_LCD_ROT   270
#endif

/* ── Audio: I2S DAC with built-in amplifier, no MCLK, no control bus ───── */
#define BOARD_SND_EN        30      /* amplifier enable, active high */
#define BOARD_I2S_BCK       28
#define BOARD_I2S_WS        29
#define BOARD_I2S_DOUT      25
#define BOARD_AUDIO_RATE    48000

/* ── SD card (SDMMC slot 0, 4-bit) ─────────────────────────────────────── */
#define BOARD_SD_D0         39
#define BOARD_SD_D1         40
#define BOARD_SD_D2         41
#define BOARD_SD_D3         42
#define BOARD_SD_CLK        43
#define BOARD_SD_CMD        44
#define BOARD_SD_LDO_CHAN   4       /* on-chip LDO that powers the SD I/O domain; must be 3.3 V */

/* ── Buttons (active low with pull-up unless noted) ────────────────────── */
#define BOARD_BTN_A         31
#define BOARD_BTN_B         5
#define BOARD_BTN_X         32
#define BOARD_BTN_Y         54
#define BOARD_BTN_LEFT      14
#define BOARD_BTN_RIGHT     12
#define BOARD_BTN_UP        13
#define BOARD_BTN_DOWN      15
#define BOARD_BTN_L         11      /* -> BSP_BTN_L1 */
#define BOARD_BTN_R         4       /* -> BSP_BTN_R1 */
#define BOARD_BTN_SW        1       /* -> BSP_BTN_HOME */

#if PICOHELD2_REV == 13
#define BOARD_BTN_START     35      /* active low */
#define BOARD_BTN_SELECT    45      /* active HIGH, pull-down */
#else
#define BOARD_BTN_SELECT    35      /* active low */
#define BOARD_BTN_START     49      /* analog (ADC2), pull-down, pressed above the threshold */
#define BOARD_START_ADC_THRESHOLD 3500   /* 12-bit raw at 12 dB attenuation */
#endif

/* ── Analog ────────────────────────────────────────────────────────────── */
#define BOARD_ANA_VBAT      19      /* 50/50 divider: the battery is twice the pin voltage */
#define BOARD_ANA_VEXT      18      /* external supply sense, not used by the BSP contract */
#define BOARD_ANA_STICK_X   16      /* analog stick: not exposed by bsp.h yet */
#define BOARD_ANA_STICK_Y   17
#define BOARD_CHG_STAT      53      /* charger status, not exposed by bsp.h */
#define BOARD_SD_DET        47      /* card detect, not used */

/* USB-C port with host support (the launcher takes gamepads and keyboards from it) */
#define BOARD_HAS_USB_HOST  1
