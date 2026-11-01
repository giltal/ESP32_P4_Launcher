/*
 * Files — browse the whole SD card: file details, text preview, delete.
 * Delete always asks first, and defaults to Cancel.
 */
#include <stdio.h>
#include <string.h>
#include "papp/papp.h"

PAPP_MANIFEST(.id = "tool.files", .name = "Files", .version = "1.0.0", .main_stack = 16384);

#define PREVIEW 480

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void show_info(const char *path)
{
    long size = papp_fs()->size(path);
    char text[300];
    if (size >= 1024 * 1024)
        snprintf(text, sizeof(text), "%s\n\n%.1f MB", path, size / (1024.0 * 1024.0));
    else if (size >= 1024)
        snprintf(text, sizeof(text), "%s\n\n%.1f KB", path, size / 1024.0);
    else
        snprintf(text, sizeof(text), "%s\n\n%ld bytes", path, size);
    papp_ui_message("File details", text);
}

static void show_preview(const char *path)
{
    static char text[PREVIEW + 1];
    papp_file_t *f = papp_fs()->open(path, "rb");
    if (!f) {
        papp_ui_message("Preview", "Cannot open this file.");
        return;
    }
    size_t n = papp_fs()->read(f, text, PREVIEW);
    papp_fs()->close(f);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == 0 || (c < 32 && c != '\n' && c != '\r' && c != '\t') || c >= 127) {
            papp_ui_message("Preview", "This isn't a text file.");
            return;
        }
        if (c == '\r' || c == '\t') text[i] = ' ';
    }
    text[n] = 0;
    papp_ui_message(base_name(path), text);
}

/* Returns true if the file is gone */
static bool ask_delete(const char *path)
{
    static const char *const items[] = { "Cancel", "Delete" };
    char title[64];
    snprintf(title, sizeof(title), "Delete %.50s?", base_name(path));
    if (papp_ui_menu(title, items, 2, 0) != 1) return false;
    if (papp_fs()->remove(path) != 0) {
        papp_ui_message("Delete", "Could not delete this file.");
        return false;
    }
    papp_log("files: deleted %s\n", path);
    return true;
}

int main(int argc, char **argv)
{
    /* A surface keeps a picture behind the menus */
    papp_surface_cfg_t cfg = { .width = 320, .height = 240, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_STRETCH, .buffers = 1 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (s) {
        int stride;
        uint16_t *px = papp_disp_lock(s, &stride);
        for (int i = 0; i < 240 * stride; i++) px[i] = PAPP_RGB565(6, 12, 28);
        papp_disp_present(s);
    }
    papp_pick_cfg_t pick = { .title = "Files", .root = "/sd", .extensions = NULL,
                             .flags = PAPP_PICK_FILES_F };
    static const char *const actions[] = { "Details", "Preview text", "Delete", "Back" };
    papp_pick_result_t r;
    while (!papp_exit_requested() && papp_ui_pick_file(&pick, &r) != PAPP_PICK_CANCEL) {
        for (bool stay = true; stay && !papp_exit_requested();) {
            char title[64];
            snprintf(title, sizeof(title), "%.60s", base_name(r.path));
            switch (papp_ui_menu(title, actions, 4, 0)) {
            case 0: show_info(r.path); break;
            case 1: show_preview(r.path); break;
            case 2: stay = !ask_delete(r.path); break;
            default: stay = false; break;
            }
        }
    }
    if (s) papp_disp_surface_destroy(s);
    return 0;
}
