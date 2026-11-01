/*
 * browse_test — UI interface test: the content browser and the message box.
 *   args: [root] [extensions] [save_dir] [picks]
 *   defaults: /sd/roms/nes .nes /sd/odroid/data/nes 1
 * Shows a test pattern surface (to check the browser draws over an app and
 * the app comes back), opens the browser `picks` times, logs each result and
 * shows it in a message box. Exit code = last action (0 cancel, 1 run,
 * 2 resume, 3 restart).
 */
#include "papp/papp.h"

PAPP_MANIFEST(.id = "test.browse", .name = "Browse test", .version = "0.1.0", .main_stack = 8192, .flags = PAPP_MF_HIDDEN);

static const char *const ACTION[] = { "cancel", "run", "resume", "restart" };

static void pattern(papp_surface_t *s, int frame)
{
    int stride;
    uint16_t *px = papp_disp_lock(s, &stride);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            px[y * stride + x] = PAPP_RGB565((x + frame) & 0xFF, y, (x ^ y) & 0xFF);
    papp_disp_present(s);
}

int main(int argc, char **argv)
{
    papp_pick_cfg_t cfg = {
        .title = "Browse test",
        .root = argc > 1 ? argv[1] : "/sd/roms/nes",
        .extensions = argc > 2 ? argv[2] : ".nes",
        .save_dir = argc > 3 ? argv[3] : "/sd/odroid/data/nes",
        .save_ext = ".sav",
    };
    int picks = 1;
    if (argc > 4) {
        picks = 0;
        for (const char *p = argv[4]; *p >= '0' && *p <= '9'; p++) picks = picks * 10 + (*p - '0');
    }

    papp_surface_cfg_t sc = { .width = 320, .height = 240, .format = PAPP_PIXFMT_RGB565,
                              .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&sc);
    if (s) pattern(s, 0);

    papp_pick_result_t r = { 0 };
    for (int i = 0; i < picks && !papp_exit_requested(); i++) {
        int64_t t0 = papp_time_us();
        int act = papp_ui_pick_file(&cfg, &r);
        papp_log("pick %d: %s after %lld ms: '%s' save '%s'\n", i + 1, ACTION[act & 3],
                 (papp_time_us() - t0) / 1000, r.path, r.save_path);
        if (act == PAPP_PICK_CANCEL) break;
        char msg[600];
        int n = 0;
        const char *p = r.path;
        for (const char *q = r.path; *q; q++) if (*q == '/') p = q + 1;
        for (; *p && n < 200; p++) msg[n++] = *p;
        msg[n] = 0;
        papp_ui_message(ACTION[act & 3], msg);
        if (s) for (int f = 0; f < 30; f++) pattern(s, f * 8);       /* the app's own picture again */
    }
    return r.action;
}
