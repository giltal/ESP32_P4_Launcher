#!/usr/bin/env python3
"""Duke Nukem 3D (Chocolate Duke3D): the RetroESP32_P4 v1 PSRAM app on the v1 compatibility host.
Data: /sd/roms/duke3d/duke3d.grp.
    python apps/duke3d/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "v1compat"))
import v1build  # noqa: E402

SRCS = {'duke3d/Engine': ['cache.c', 'display.c', 'draw.c', 'dummy_multi.c', 'engine.c', 'filesystem.c', 'fixedPoint_math.c', 'tiles.c'], 'duke3d/Game': ['actors.c', 'animlib.c', 'config.c', 'console.c', 'control.c', 'cvar_defs.c', 'cvars.c', 'game.c', 'gamedef.c', 'global.c', 'keyboard.c', 'menues.c', 'player.c', 'premap.c', 'rts.c', 'scriplib.c', 'sector.c', 'sounds.c', 'dummy_audiolib.c'], 'duke3d/audiolib': ['fx_man.c', 'dsl.c', 'll_man.c', 'multivoc.c', 'mv_mix.c', 'mvreverb.c', 'nodpmi.c', 'pitch.c', 'user.c', 'nomusic.c']}
GLUE = ['papp_main.c', 'papp_syscalls.c', 'papp_sdl_video.c', 'papp_sdl_audio.c', 'papp_sdl_event.c', 'papp_sdl_system.c']
FAST = '-DIRAM_ATTR=__attribute__((section(".fast.text")))'     # hot paths to internal SRAM

srcs = [os.path.join(HERE, d, f) for d, files in SRCS.items() for f in files] + [os.path.join(HERE, f) for f in GLUE]
v1build.build('duke3d', srcs, includes=[os.path.join(HERE, i) for i in ['compat', 'duke3d/Engine', 'duke3d/Game', 'duke3d/audiolib']],
              defines=['-DEXT_RAM_ATTR=', '-DPLATFORM_SUPPORTS_SDL=1', '-DPLATFORM_ESP32=1', '-DSTUB_NETWORKING=1', '-fcommon'] + [FAST, "-O3"], menu_button='0', volume_button='0',
              rename_main=True, extra_ldflags=['-Wl,--allow-multiple-definition'], icon_dir=HERE, title='Duke Nukem 3D')
