#!/usr/bin/env python3
"""Doom (prboom-go) as a PAPP app: the RetroESP32_P4 v1 PSRAM app, unchanged, on
the v1 compatibility host (apps/common/v1compat).
    python apps/doom/build.py
Needs /sd/roms/doom/doom.wad; saves in /sd/saves/doom (create it)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "v1compat"))
import v1build  # noqa: E402

ENGINE = os.path.join(HERE, "prboom")
GAME = ["am_map.c", "d_client.c", "d_deh.c", "d_items.c", "d_main.c", "d_server.c", "dbopl.c", "doomdef.c",
        "doomstat.c", "dstrings.c", "f_finale.c", "f_wipe.c", "g_game.c", "hu_lib.c", "hu_stuff.c", "info.c",
        "lprintf.c", "m_argv.c", "m_bbox.c", "m_cheat.c", "m_menu.c", "m_misc.c", "m_random.c", "midifile.c",
        "mus2mid.c", "opl.c", "opl_queue.c", "oplplayer.c", "p_ceilng.c", "p_doors.c", "p_enemy.c", "p_floor.c",
        "p_genlin.c", "p_inter.c", "p_lights.c", "p_map.c", "p_maputl.c", "p_mobj.c", "p_plats.c", "p_pspr.c",
        "p_saveg.c", "p_setup.c", "p_sight.c", "p_spec.c", "p_switch.c", "p_telept.c", "p_tick.c", "p_user.c",
        "r_bsp.c", "r_data.c", "r_demo.c", "r_draw.c", "r_filter.c", "r_fps.c", "r_main.c", "r_patch.c",
        "r_plane.c", "r_segs.c", "r_sky.c", "r_things.c", "s_sound.c", "sounds.c", "st_lib.c", "st_stuff.c",
        "tables.c", "v_video.c", "version.c", "w_wad.c", "wi_stuff.c", "z_zone.c"]
GLUE = ["papp_main.c", "papp_i_video.c", "papp_i_sound.c", "papp_i_system.c", "papp_rg_stubs.c", "papp_syscalls.c"]

srcs = [os.path.join(ENGINE, f) for f in GAME] + [os.path.join(HERE, f) for f in GLUE]
v1build.build("doom", srcs, includes=[os.path.join(HERE, "compat"), ENGINE],
              defines=["-DRETRO_GO=1", "-DHAVE_CONFIG_H=1", "-DIRAM_ATTR=", "-fno-common", "-O2"],
              menu_button="PAPP_BTN_X", volume_button="0", rename_main=True, icon_dir=HERE, title='Doom')
