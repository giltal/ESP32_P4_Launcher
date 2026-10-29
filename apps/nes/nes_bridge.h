/*
 * Bridge between the nofrendo core (its own 4-byte enum `bool`, no SDK
 * headers allowed) and the PAPP side in main.c. Plain C types only.
 */
#pragma once

#define NB_A       0x01
#define NB_B       0x02
#define NB_SELECT  0x04
#define NB_START   0x08
#define NB_UP      0x10
#define NB_DOWN    0x20
#define NB_LEFT    0x40
#define NB_RIGHT   0x80
#define NB_MENU    0x100      /* X: in-game menu */

/* In-game menu choices (nb_menu) */
enum { NB_M_RESUME, NB_M_SAVE, NB_M_LOAD, NB_M_RESTART, NB_M_DELETE, NB_M_EXIT };

void         nb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
long long    nb_time_us(void);
unsigned int nb_free_internal(void);
int          nb_exit_requested(void);

/* Display surface, audio stream and video thread for one game */
int          nb_av_init(int sample_rate);
void         nb_av_shutdown(void);
void         nb_audio_write(const short *mono, int samples);
void         nb_set_palette(int index, int r, int g, int b);
void         nb_video_frame(unsigned char *const *lines);    /* 224 lines of 256 colour indices */
unsigned int nb_buttons(int player);                        /* NB_* held by player 0 / 1 */

/* Pauses audio, shows the menu over the game, waits for the buttons to be
 * released; note (or NULL) goes in the title. Returns NB_M_*. */
int          nb_menu(const char *note);
void         nb_delete_save(void);

/* ROM image loaded by main.c */
extern char *nb_rom;
