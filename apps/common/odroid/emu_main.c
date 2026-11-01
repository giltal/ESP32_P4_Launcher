/*
 * Shared main() of the emulator apps built on the odroid layer. Each app is
 * compiled with:
 *   EMU_ID, EMU_NAME          manifest id / name ("emu.gb", "Game Boy")
 *   EMU_ROOT, EMU_EXTS        ROM folder and extensions (".gb")
 *   EMU_SAVE_DIR              RetroESP32 save folder (/sd/odroid/data/<sys>)
 *   EMU_SAVE_NOEXT            1 if saves drop the ROM extension (SNES, Genesis)
 *   EMU_EXCLUDE               optional: files the browser hides ("neogeo.zip,BIOS.col")
 *   EMU_NO_DIRS               1: the browser lists no sub-folders (Neo Geo caches)
 *   EMU_RUN                   the emulator's run function: void EMU_RUN(const char *rom)
 *   EMU_STACK                 main stack bytes (> 32 KB goes to PSRAM)
 * Flow: launcher browser -> RomFilePath + StartAction (resume) -> EMU_RUN.
 * The emulator's own in-game menu ends the game. Started from the browser, the
 * app then exits with PAPP_EXIT_RELAUNCH: the launcher starts it again, fresh,
 * in the browser (the cores keep static state, so one game per load). Started
 * with a ROM argument (DevLink), it just exits.
 */
#include "papp/papp.h"

#include <setjmp.h>
#include <string.h>

#include "odroid_audio.h"
#include "odroid_settings.h"

#ifndef EMU_STACK
#define EMU_STACK 32768
#endif
#ifndef EMU_SAVE_NOEXT
#define EMU_SAVE_NOEXT 0
#endif
#ifndef EMU_NO_DIRS
#define EMU_NO_DIRS 0
#endif
#ifdef EMU_EXCLUDE
#define EMU_EXCLUDE_F PAPP_PICK_EXCLUDE_F
#else
#define EMU_EXCLUDE NULL
#define EMU_EXCLUDE_F 0
#endif

PAPP_MANIFEST(.id = EMU_ID, .name = EMU_NAME, .version = "0.1.0", .main_stack = EMU_STACK);

void EMU_RUN(const char *rom);
void odroid_papp_set_rom(const char *path, jmp_buf *exit_jb);
void odroid_papp_display_close(void);
extern int odroid_papp_exit_code;

static jmp_buf s_exit;

int main(int argc, char **argv)
{
    static char rom[256];
    int resume = 0;
    if (argc > 1) {                                    /* DevLink: -- <rom> [resume] */
        strncpy(rom, argv[1], sizeof(rom) - 1);
        resume = argc > 2 && strcmp(argv[2], "resume") == 0;
    } else {
        papp_pick_cfg_t cfg = {
            .title = EMU_NAME,
            .root = EMU_ROOT,
            .extensions = EMU_EXTS,
            .save_dir = EMU_SAVE_DIR,
            .save_ext = ".sav",
            .flags = (EMU_SAVE_NOEXT ? PAPP_PICK_SAVE_NOEXT_F : 0) | EMU_EXCLUDE_F |
                     (EMU_NO_DIRS ? PAPP_PICK_NO_DIRS_F : 0),
            .exclude = EMU_EXCLUDE,
        };
        static papp_pick_result_t r;
        int act = papp_ui_pick_file(&cfg, &r);
        if (act == PAPP_PICK_CANCEL) return 0;
        strncpy(rom, r.path, sizeof(rom) - 1);
        resume = act == PAPP_PICK_RESUME;
    }
    papp_log("%s: %s%s\n", EMU_NAME, rom, resume ? " (resume)" : "");
    odroid_settings_RomFilePath_set(rom);
    odroid_settings_StartAction_set(resume ? ODROID_START_ACTION_RESTART : ODROID_START_ACTION_NORMAL);
    odroid_papp_set_rom(rom, &s_exit);
    odroid_papp_exit_code = argc > 1 ? 0 : PAPP_EXIT_RELAUNCH;
    if (setjmp(s_exit) == 0) EMU_RUN(rom);
    odroid_audio_terminate();
    odroid_papp_display_close();
    return odroid_papp_exit_code;
}
