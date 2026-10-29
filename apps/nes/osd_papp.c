/*
 * nofrendo OSD layer for the PAPP launcher (replaces RetroESP32's
 * esp32/osd.c + esp32/video_audio.c). nofrendo headers only: everything that
 * needs the SDK goes through nes_bridge.h (main.c).
 */
#include <stdio.h>
#include <string.h>
#include <noftypes.h>
#include <bitmap.h>
#include <nofconfig.h>
#include <event.h>
#include <log.h>
#include <nes.h>
#include <nesinput.h>
#include <osd.h>
#include <nofrendo.h>
#include "nesstate.h"
#include "nes_bridge.h"

#define SAMPLE_RATE   32000
#define FRAG          (SAMPLE_RATE / NES_REFRESH_RATE + 1)

volatile bool nofrendo_quit_flag = false;
bool forceConsoleReset = false;            /* soft-reset after load_sram (fresh start) */

char *osd_getromdata(void)
{
   return nb_rom;
}

/* ── Audio: nofrendo renders one frame of mono samples; main.c queues them ── */

static void (*audio_callback)(void *buffer, int length);
static short s_audio[FRAG];

void osd_setsound(void (*playfunc)(void *buffer, int length))
{
   audio_callback = playfunc;
}

void osd_getsoundinfo(sndinfo_t *info)
{
   info->sample_rate = SAMPLE_RATE;
   info->bps = 16;
}

/* Called by nes_emulate once per frame; the blocking write paces emulation */
void do_audio_frame(void)
{
   if (!audio_callback)
      return;
   int n = SAMPLE_RATE / NES_REFRESH_RATE;
   audio_callback(s_audio, n);
   nb_audio_write(s_audio, n);
}

int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
   return 0;
}

/* ── Video ─────────────────────────────────────────────────────────────── */

static int vd_init(int width, int height)  { return 0; }
static void vd_shutdown(void)              { }
static int vd_set_mode(int width, int height) { return 0; }
static void vd_clear(uint8 color)          { }

static void vd_set_palette(rgb_t *pal)
{
   for (int i = 0; i < 256; i++)
      nb_set_palette(i, pal[i].r, pal[i].g, pal[i].b);
}

static char s_dummy_fb[1];
static bitmap_t *s_bitmap;

static bitmap_t *vd_lock_write(void)
{
   s_bitmap = bmp_createhw((uint8 *)s_dummy_fb, NES_SCREEN_WIDTH, NES_VISIBLE_HEIGHT, NES_SCREEN_WIDTH * 2);
   return s_bitmap;
}

static void vd_free_write(int num_dirties, rect_t *dirty_rects)
{
   bmp_destroy(&s_bitmap);
}

static void vd_custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
   if (bmp && bmp->line[0])
      nb_video_frame((unsigned char *const *)bmp->line);
}

static viddriver_t s_driver = {
   "PAPP DISP",
   vd_init,
   vd_shutdown,
   vd_set_mode,
   vd_set_palette,
   vd_clear,
   vd_lock_write,
   vd_free_write,
   vd_custom_blit,
   false,
};

void osd_getvideoinfo(vidinfo_t *info)
{
   info->default_width = NES_SCREEN_WIDTH;
   info->default_height = NES_VISIBLE_HEIGHT;
   info->driver = &s_driver;
}

void osd_togglefullscreen(int code)
{
}

/* ── Input ─────────────────────────────────────────────────────────────── */

extern int nes_load_save;

/* Saves, then ends nes_emulate: back to main.c (browser, or app exit) */
static void quit(void)
{
   nb_log("nes: leaving the game, saving state\n");
   nes_poweroff();
   save_sram();
   nofrendo_quit_flag = true;
}

/* X: the in-game menu (RetroESP32's MENU button), drawn by the launcher */
static void game_menu(void)
{
   const char *note = NULL;
   for (;;)
   {
      int choice = nb_menu(note);
      note = NULL;
      switch (choice)
      {
      case NB_M_SAVE:
         save_sram();
         note = "saved";
         continue;
      case NB_M_DELETE:
         nb_delete_save();
         note = "save deleted";
         continue;
      case NB_M_LOAD:
         nes_load_save = 1;
         load_sram();
         return;
      case NB_M_RESTART:
         nes_reset(HARD_RESET);
         return;
      case NB_M_EXIT:
         quit();
         return;
      default:
         return;
      }
   }
}

void osd_getinput(void)
{
   static const struct { unsigned int bit; int ev[2]; } map[] = {
      { NB_A,      { event_joypad1_a,      event_joypad2_a } },
      { NB_B,      { event_joypad1_b,      event_joypad2_b } },
      { NB_SELECT, { event_joypad1_select, event_joypad2_select } },
      { NB_START,  { event_joypad1_start,  event_joypad2_start } },
      { NB_UP,     { event_joypad1_up,     event_joypad2_up } },
      { NB_DOWN,   { event_joypad1_down,   event_joypad2_down } },
      { NB_LEFT,   { event_joypad1_left,   event_joypad2_left } },
      { NB_RIGHT,  { event_joypad1_right,  event_joypad2_right } },
   };
   static unsigned int old[2];

   if (nofrendo_quit_flag)
      return;
   if (nb_exit_requested())
   {
      quit();
      return;
   }
   for (int p = 0; p < 2; p++)
   {
      unsigned int now = nb_buttons(p), chg = now ^ old[p];
      old[p] = now;
      if (chg & now & NB_MENU)
      {
         game_menu();
         old[0] = nb_buttons(0);
         old[1] = nb_buttons(1);
         return;
      }
      for (unsigned int i = 0; chg && i < sizeof(map) / sizeof(map[0]); i++)
      {
         if (!(chg & map[i].bit))
            continue;
         event_t h = event_get(map[i].ev[p]);
         if (h)
            h((now & map[i].bit) ? INP_STATE_MAKE : INP_STATE_BREAK);
      }
   }
}

void osd_getmouse(int *x, int *y, int *button)
{
   *x = *y = *button = 0;
}

/* ── Files, startup, shutdown ──────────────────────────────────────────── */

char configfilename[] = "na";

int osd_main(int argc, char *argv[])
{
   config.filename = configfilename;
   return main_loop(argv[0], system_nes);
}

void osd_fullname(char *fullname, const char *shortname)
{
   strncpy(fullname, shortname, PATH_MAX);
}

char *osd_newextension(char *string, char *ext)
{
   return string;
}

int osd_makesnapname(char *filename, int len)
{
   return -1;
}

static int logprint(const char *string)
{
   nb_log("%s", string);
   return 0;
}

int osd_init(void)
{
   log_chain_logfunc(logprint);
   audio_callback = NULL;
   return nb_av_init(SAMPLE_RATE) ? 0 : -1;
}

void osd_shutdown(void)
{
   audio_callback = NULL;
   nb_av_shutdown();
}
