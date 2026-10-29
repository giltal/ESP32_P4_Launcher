#pragma once

#include <stdbool.h>

/* Redraw the launcher status screen (double-buffered, safe from any task). */
void status_screen_draw(void);
void status_screen_set_last_app(const char *text);
void status_screen_set_audio(const char *text);
void status_screen_set_touch(bool down, int x, int y);
void status_screen_set_input(const char *text);
