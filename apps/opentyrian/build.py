#!/usr/bin/env python3
"""OpenTyrian: the RetroESP32_P4 v1 PSRAM app on the v1 compatibility host. Data: /sd/tyrian/data.
    python apps/opentyrian/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "v1compat"))
import v1build  # noqa: E402

SRCS = {'opentyrian': ['animlib.c', 'arg_parse.c', 'backgrnd.c', 'config.c', 'config_file.c', 'destruct.c', 'editship.c', 'episodes.c', 'file.c', 'font.c', 'fonthand.c', 'game_menu.c', 'helptext.c', 'joystick.c', 'jukebox.c', 'keyboard.c', 'lds_play.c', 'loudness.c', 'lvllib.c', 'lvlmast.c', 'mainint.c', 'menus.c', 'mouse.c', 'mtrand.c', 'musmast.c', 'network.c', 'nortsong.c', 'nortvars.c', 'opentyr.c', 'opl.c', 'palette.c', 'params.c', 'pcxload.c', 'pcxmast.c', 'picload.c', 'player.c', 'scroller.c', 'setup.c', 'shots.c', 'sizebuf.c', 'sndmast.c', 'sprite.c', 'starlib.c', 'std_support.c', 'tyrian2.c', 'varz.c', 'vga256d.c', 'vga_palette.c', 'video.c', 'video_scale.c', 'video_scale_hqNx.c', 'xmas.c']}
GLUE = ['papp_main.c', 'papp_sdl_video.c', 'papp_sdl_audio.c', 'papp_sdl_event.c', 'papp_sdl_system.c', 'papp_syscalls.c']
FAST = '-DIRAM_ATTR=__attribute__((section(".fast.text")))'     # hot paths to internal SRAM

srcs = [os.path.join(HERE, d, f) for d, files in SRCS.items() for f in files] + [os.path.join(HERE, f) for f in GLUE]
v1build.build('opentyrian', srcs, includes=[os.path.join(HERE, i) for i in ['compat', 'opentyrian_sdl', 'opentyrian']],
              defines=['-fno-common'] + [FAST, "-O2"], menu_button='PAPP_BTN_X', volume_button='PAPP_BTN_Y',
              rename_main=True, extra_ldflags=[], icon_dir=HERE, title='OpenTyrian')
