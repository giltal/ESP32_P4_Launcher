/* Guition ESP32-P4 4.3" (480x800 ST7701S MIPI-DSI, GT911, ES8311) pin map.
 * Taken from RetroESP32_P4 components/odroid/include/pins_config.h. */
#pragma once

#define BOARD_LCD_H_RES     480
#define BOARD_LCD_V_RES     800
#define BOARD_LCD_BL_GPIO   23
#define BOARD_LCD_RST_GPIO  5
#define BOARD_DSI_PHY_LDO_CHAN 3
#define BOARD_DSI_PHY_LDO_MV   2500

#define BOARD_TP_I2C_SDA    7
#define BOARD_TP_I2C_SCL    8

#define BOARD_I2S_MCLK      13
#define BOARD_I2S_BCLK      12
#define BOARD_I2S_WS        10
#define BOARD_I2S_DOUT      9
#define BOARD_I2S_DIN       48
#define BOARD_AUDIO_PA      11

#define BOARD_BATTERY_ADC   53

#define BOARD_SD_CLK        43
#define BOARD_SD_CMD        44
#define BOARD_SD_D0         39
#define BOARD_SD_D1         40
#define BOARD_SD_D2         41
#define BOARD_SD_D3         42
#define BOARD_SD_LDO_CHAN   4

#define BOARD_I2C_PORT      1       /* shared by GT911 and ES8311 */
#define BOARD_TP_RST        -1
#define BOARD_TP_INT        -1
#define BOARD_AUDIO_RATE    48000

/* Custom GPIO gamepad add-on (RetroESP32_P4 Phase 40) and Atari paddle.
 * Analog pins are ADC2; digital buttons are active high with physical pull-downs. */
#define BOARD_PAD_JOY_LR    49      /* ~3300 left, ~1650 right */
#define BOARD_PAD_JOY_UD    50      /* ~3300 up,   ~1650 down  */
#define BOARD_PAD_AB        52      /* ~3300 A,    ~1650 B     */
#define BOARD_PAD_DETECT    29      /* presence pin (pull-up reads 0 when the pad is fitted) */
#define BOARD_PAD_R1        29      /* right shoulder (checked on the hardware) */
#define BOARD_PAD_L1        30      /* left shoulder */
#define BOARD_PAD_X         33
#define BOARD_PAD_Y         34
#define BOARD_PAD_START     28
#define BOARD_PAD_SELECT    32
#define BOARD_PADDLE_ADC    51
