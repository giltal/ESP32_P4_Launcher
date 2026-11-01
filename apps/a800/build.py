#!/usr/bin/env python3
"""Atari 800 / XL / 5200 app (atari800, from RetroESP32_P4).
    python apps/a800/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['afile.cpp', 'antic.cpp', 'artifact.cpp', 'atari.cpp', 'binload.cpp', 'cartridge.cpp', 'cassette.cpp', 'cfg.cpp', 'compfile.cpp', 'cpu.cpp', 'crc32.cpp', 'devices.cpp', 'esc.cpp', 'gtia.cpp', 'img_tape.cpp', 'input.cpp', 'libatari800_exit.cpp', 'libatari800_init.cpp', 'libatari800_main.cpp', 'libatari800_sound.cpp', 'libatari800_statesav.cpp', 'libatari800_video.cpp', 'log.cpp', 'memory.cpp', 'pbi.cpp', 'pia.cpp', 'pokey.cpp', 'pokeysnd.cpp', 'roms_altirra_5200_os.cpp', 'roms_altirra_basic.cpp', 'roms_altirraos_800.cpp', 'roms_altirraos_xl.cpp', 'screen.cpp', 'sio.cpp', 'sndsave.cpp', 'sound.cpp', 'statesav.cpp', 'sysrom.cpp', 'util.cpp', 'atari800_run.cpp']
DEFS = ['ESP_PLATFORM=1', '_GNU_SOURCE']   # as in the IDF build: disk images and XEX cached in PSRAM (fmemopen)

build_emu('a800', CORE, SRCS, dict(id="emu.a800", name='Atari 800', root="/sd/roms/a800", exts='.xex,.atr,.a52,.car,.rom,.bin,.cas',
                                 save_dir="/sd/odroid/data/a800", run='atari800_run'),
          defines=DEFS, includes=['.', 'include'], opt='-O2', cxx=True)
