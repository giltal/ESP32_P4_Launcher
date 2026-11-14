/*
 * I2S DAC with built-in amplifier (no MCLK, no control bus), I2S0, configured once at BOARD_AUDIO_RATE
 * (16-bit stereo) and never reconfigured. There is no codec to set a volume on, so the volume is applied
 * to the samples here. The amplifier is enabled with BOARD_SND_EN once the I2S clocks are running.
 */
#include "board_internal.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

// This resets the GPIO for DAT output on the i2s peripheral, because
// it get's de-configured by something (needs further investigation)
#define I2S_PIN_WA

#ifdef I2S_PIN_WA
#include "esp_rom_gpio.h"
#include "soc/i2s_periph.h"
#endif

static const char *TAG = "audio";

#define CHUNK_FRAMES 256

static i2s_chan_handle_t s_tx;
static volatile int s_gain = 256 * 60 * 60 / 10000;     /* 0..256, from the volume percent (squared curve) */

esp_err_t bsp_audio_init(void)
{
    if (s_tx) return ESP_OK;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;         /* underrun plays silence, not the last buffer */
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 480;       /* 10 ms per descriptor at 48 kHz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BOARD_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCK,
            .ws   = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

    gpio_set_level(BOARD_SND_EN, 1);    /* amplifier on, clocks are running */
    board_set_cap(BSP_CAP_AUDIO);
    ESP_LOGI(TAG, "I2S DAC ready, %d Hz 16-bit stereo", BOARD_AUDIO_RATE);
    return ESP_OK;
}

int bsp_audio_rate(void)
{
    return BOARD_AUDIO_RATE;
}

size_t bsp_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    if (!s_tx) return 0;

#ifdef I2S_PIN_WA
    /* WA: take GPIO25 back for the I2S data signal */
    static bool dumped;
    if (!dumped) {
        dumped = true;
        gpio_dump_io_configuration(stdout, 1ULL << BOARD_I2S_DOUT);   /* state as found */
        
        esp_rom_gpio_pad_select_gpio(BOARD_I2S_DOUT);                     /* IO MUX function back to GPIO */
        gpio_pullup_dis(BOARD_I2S_DOUT);
        gpio_pulldown_dis(BOARD_I2S_DOUT);
        gpio_set_direction(BOARD_I2S_DOUT, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(BOARD_I2S_DOUT, i2s_periph_signal[I2S_NUM_0].data_out_sig, false, false);
        gpio_dump_io_configuration(stdout, 1ULL << BOARD_I2S_DOUT);   /* state as found */
    }
#endif

  int gain = s_gain;
  
  if (gain < 256) {
      int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
      int16_t buf[CHUNK_FRAMES * 2];
      size_t done = 0;

      while (done < frames) {
          size_t n = frames - done;
          if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;
          const int16_t *src = stereo + done * 2;
          if (gain >= 256) {
              for (size_t i = 0; i < n * 2; i++) buf[i] = src[i];
          } else {
              for (size_t i = 0; i < n * 2; i++) buf[i] = (int16_t)((src[i] * gain) >> 8);
          }

          int64_t left_us = deadline - esp_timer_get_time();
          uint32_t wait_ms = left_us > 0 ? (uint32_t)(left_us / 1000) : 0;
          size_t written = 0;
          i2s_channel_write(s_tx, buf, n * 4, &written, pdMS_TO_TICKS(wait_ms));
          done += written / 4;
          if (written < n * 4) break;     /* timed out */
      }
      return done;
  } else {
      size_t written = 0;
      i2s_channel_write(s_tx, stereo, frames * 4, &written, pdMS_TO_TICKS(timeout_ms));
      return written;
  }
   
}

void bsp_audio_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_gain = 256 * percent * percent / 10000;
}
