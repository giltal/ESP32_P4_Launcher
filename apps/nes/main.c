/*
 * NES — nofrendo as a PAPP app.
 *   args: (none)                 browse /sd/roms/nes and play the chosen ROM
 *         <rom> [resume]         play <rom> directly (DevLink testing)
 *
 * Video: a 256x224 surface scaled 2x by the compositor; a worker thread turns
 * nofrendo's 8-bit frames into RGB565 and presents them, so waiting for the
 * display never stalls emulation. Audio: one 32 kHz mono stream; its
 * blocking write paces the emulator. Saves: the browser's save path
 * (RetroESP32 layout, /sd/odroid/data/nes/<rom>.sav). X opens the in-game
 * menu (resume / save / load / restart / delete save / exit); leaving a game
 * saves it and returns to the browser; backing out of the browser (or HOME)
 * ends the app.
 * This file is the SDK side; osd_papp.c is the nofrendo side (nes_bridge.h).
 */
#include "papp/papp.h"
#include "nes_bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "emu.nes", .name = "NES", .version = "0.1.0", .main_stack = 32768);

#define NES_W   256
#define NES_H   224

int nofrendo_main(int argc, char *argv[]);
extern const char *nes_save_path;
extern int nes_load_save;
extern volatile int nofrendo_quit_flag;     /* nofrendo bool: 4-byte enum */
extern int forceConsoleReset;

char *nb_rom;

/* ── Bridge: misc ──────────────────────────────────────────────────────── */

void nb_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    papp_log("%s", buf);
}

long long    nb_time_us(void)        { return papp_time_us(); }
unsigned int nb_free_internal(void)  { return papp_mem_free_size(PAPP_MEM_FAST); }
int          nb_exit_requested(void) { return papp_exit_requested(); }

/* ── Bridge: video ─────────────────────────────────────────────────────── */

static papp_surface_t *s_surface;
static uint16_t s_pal[256];
static uint8_t *s_frame[2];                 /* 8-bit frames handed to the video thread */
static int s_next;
static papp_queue_t *s_vq;                  /* frame index; -1 = stop */
static papp_thread_t *s_video;
static uint32_t s_shown, s_dropped;

void nb_set_palette(int i, int r, int g, int b)
{
    s_pal[i & 255] = PAPP_RGB565(r, g, b);
}

static int video_thread(void *arg)
{
    int idx;
    while (papp_queue_recv(s_vq, &idx, PAPP_WAIT_FOREVER) && idx >= 0) {
        int stride;
        uint16_t *dst = papp_disp_lock(s_surface, &stride);
        const uint8_t *src = s_frame[idx];
        for (int y = 0; y < NES_H; y++, dst += stride, src += NES_W)
            for (int x = 0; x < NES_W; x += 4) {
                dst[x] = s_pal[src[x]];
                dst[x + 1] = s_pal[src[x + 1]];
                dst[x + 2] = s_pal[src[x + 2]];
                dst[x + 3] = s_pal[src[x + 3]];
            }
        papp_disp_present(s_surface);
        s_shown++;
    }
    return 0;
}

void nb_video_frame(unsigned char *const *lines)
{
    int idx = s_next;
    s_next ^= 1;
    for (int y = 0; y < NES_H; y++)
        memcpy(s_frame[idx] + y * NES_W, lines[y], NES_W);
    /* Mailbox of one: replace a frame the video thread hasn't taken yet */
    int stale;
    if (papp_queue_recv(s_vq, &stale, 0)) s_dropped++;
    papp_queue_send(s_vq, &idx, 0);
}

/* ── Bridge: audio ─────────────────────────────────────────────────────── */

static papp_audio_stream_t *s_stream;

void nb_audio_write(const short *mono, int samples)
{
    if (s_stream) papp_audio_write(s_stream, mono, samples, 200);
}

int nb_av_init(int sample_rate)
{
    papp_surface_cfg_t sc = { .width = NES_W, .height = NES_H, .format = PAPP_PIXFMT_RGB565,
                              .scale = PAPP_SCALE_INTEGER, .buffers = 2 };
    s_surface = papp_disp_surface_create(&sc);
    s_frame[0] = papp_alloc(NES_W * NES_H, PAPP_MEM_ANY, 64);
    s_frame[1] = papp_alloc(NES_W * NES_H, PAPP_MEM_ANY, 64);
    s_vq = papp_queue_create(1, sizeof(int));
    papp_audio_cfg_t ac = { .rate = sample_rate, .channels = 1, .format = PAPP_AUDIO_S16 };
    s_stream = papp_audio_open(&ac);
    if (!s_surface || !s_frame[0] || !s_frame[1] || !s_vq) {
        papp_log("nes: out of memory for video\n");
        return 0;
    }
    papp_thread_cfg_t tc = { .name = "nes_video", .stack_size = 4096, .prio = PAPP_PRIO_NORMAL, .core = 1 };
    s_video = papp_thread_create(video_thread, NULL, &tc);
    s_shown = s_dropped = 0;
    return s_video != NULL;
}

void nb_av_shutdown(void)
{
    if (s_video) {
        int stop = -1, stale;
        papp_queue_recv(s_vq, &stale, 0);
        papp_queue_send(s_vq, &stop, 100);
        papp_thread_join(s_video, 1000, NULL);
        s_video = NULL;
    }
    papp_log("nes: %lu frames shown, %lu dropped\n", (unsigned long)s_shown, (unsigned long)s_dropped);
    if (s_stream) { papp_audio_close(s_stream); s_stream = NULL; }
    if (s_vq) { papp_queue_destroy(s_vq); s_vq = NULL; }
    papp_free(s_frame[0]);
    papp_free(s_frame[1]);
    s_frame[0] = s_frame[1] = NULL;
    if (s_surface) { papp_disp_surface_destroy(s_surface); s_surface = NULL; }
}

/* ── Bridge: input ─────────────────────────────────────────────────────── */

unsigned int nb_buttons(int player)
{
    papp_gamepad_t g;
    if (!papp_input_gamepad(player, &g)) return 0;
    uint32_t b = g.buttons;
    unsigned int r = 0;
    if (b & PAPP_BTN_A)      r |= NB_A;
    if (b & PAPP_BTN_B)      r |= NB_B;
    if (b & PAPP_BTN_SELECT) r |= NB_SELECT;
    if (b & PAPP_BTN_START)  r |= NB_START;
    if ((b & PAPP_BTN_UP)    || g.ly < -16000) r |= NB_UP;
    if ((b & PAPP_BTN_DOWN)  || g.ly > 16000)  r |= NB_DOWN;
    if ((b & PAPP_BTN_LEFT)  || g.lx < -16000) r |= NB_LEFT;
    if ((b & PAPP_BTN_RIGHT) || g.lx > 16000)  r |= NB_RIGHT;
    if (b & PAPP_BTN_X)      r |= NB_MENU;
    return r;
}

/* ── Bridge: in-game menu ──────────────────────────────────────────────── */

static bool save_exists(void)
{
    if (!nes_save_path || !*nes_save_path) return false;
    FILE *f = fopen(nes_save_path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

int nb_menu(const char *note)
{
    const char *items[6];
    int ids[6], n = 0;
    bool saved = save_exists();
    items[n] = "Resume game";   ids[n++] = NB_M_RESUME;
    items[n] = "Save game";     ids[n++] = NB_M_SAVE;
    if (saved) { items[n] = "Load saved game"; ids[n++] = NB_M_LOAD; }
    items[n] = "Restart game";  ids[n++] = NB_M_RESTART;
    if (saved) { items[n] = "Delete save";     ids[n++] = NB_M_DELETE; }
    items[n] = "Exit game";     ids[n++] = NB_M_EXIT;

    char title[48];
    if (note) snprintf(title, sizeof(title), "Game menu - %s", note);
    else snprintf(title, sizeof(title), "Game menu");
    if (s_stream) papp_audio_pause(s_stream, true);
    int k = papp_ui_menu(title, items, n, 0);
    if (s_stream) papp_audio_pause(s_stream, false);
    /* don't let the button that closed the menu reach the game */
    for (int t = 0; t < 50 && (nb_buttons(0) | nb_buttons(1)); t++) papp_delay_ms(10);
    return k < 0 ? NB_M_RESUME : ids[k];
}

void nb_delete_save(void)
{
    if (nes_save_path && *nes_save_path) remove(nes_save_path);
}

/* ── App ───────────────────────────────────────────────────────────────── */

static long load_rom(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    nb_rom = size > 16 ? papp_alloc(size, PAPP_MEM_LARGE, 0) : NULL;
    if (!nb_rom || fread(nb_rom, 1, size, f) != (size_t)size) size = -1;
    fclose(f);
    return size;
}

static int play(const char *rom, const char *save, int resume)
{
    long size = load_rom(rom);
    if (size < 0) {
        papp_ui_message("Cannot load ROM", rom);
        return -1;
    }
    papp_log("nes: %s (%ld bytes), %s\n", rom, size, resume ? "resuming" : "fresh start");
    nes_save_path = save;
    nes_load_save = resume;
    forceConsoleReset = !resume;
    nofrendo_quit_flag = 0;

    const char *base = strrchr(rom, '/');
    char *name = strdup(base ? base + 1 : rom);      /* nofrendo frees it at shutdown */
    char *args[1] = { name };
    nofrendo_main(1, args);

    papp_free(nb_rom);
    nb_rom = NULL;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        static char save[300];
        const char *base = strrchr(argv[1], '/');
        snprintf(save, sizeof(save), "/sd/odroid/data/nes/%s.sav", base ? base + 1 : argv[1]);
        return play(argv[1], save, argc > 2 && strcmp(argv[2], "resume") == 0);
    }

    papp_pick_cfg_t cfg = {
        .title = "NES",
        .root = "/sd/roms/nes",
        .extensions = ".nes",
        .save_dir = "/sd/odroid/data/nes",
        .save_ext = ".sav",
    };
    static papp_pick_result_t r;
    while (!papp_exit_requested()) {
        int act = papp_ui_pick_file(&cfg, &r);
        if (act == PAPP_PICK_CANCEL) break;
        play(r.path, r.save_path, act == PAPP_PICK_RESUME);     /* Exit game: back here */
    }
    return 0;
}
