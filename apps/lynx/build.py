#!/usr/bin/env python3
"""Atari Lynx app (handy, from RetroESP32_P4).
    python apps/lynx/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['handy_run.cpp', 'cart.cpp', 'eeprom.cpp', 'lynxdec.cpp', 'memmap.cpp', 'mikie.cpp', 'ram.cpp', 'rom.cpp', 'susie.cpp', 'system.cpp']
DEFS = ['MY_NO_STATIC', 'MY_KEYS_IN_VIDEO', 'MY_RETRO_LOOP', 'MY_SYSTEM_LOOP=2', 'MY_MEM_MODE_V2', 'MY_AUDIO_MODE=2', 'MY_ALLOC', 'GIT_VERSION=\\"\\"']

build_emu('lynx', CORE, SRCS, dict(id="emu.lynx", name='Atari Lynx', root="/sd/roms/lynx", exts='.lnx',
                                 save_dir="/sd/odroid/data/lynx", run='handy_run'),
          defines=DEFS, includes=['.'], opt='-Os', cxx=True,
          cflags=['-include', 'stdio.h', '-include', 'stdlib.h', '-include', 'string.h'])
