#!/usr/bin/env python3
"""Atari 7800 app (prosystem, from RetroESP32_P4).
    python apps/a78/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['Bios.c', 'Cartridge.c', 'Database.c', 'Hash.c', 'Maria.c', 'Memory.c', 'Palette.c', 'Pokey.c', 'ProSystem.c', 'Region.c', 'Riot.c', 'Sally.c', 'Tia.c', 'prosystem_run.c']
DEFS = []
INCS = ['include', '.']

for app, name, exts in [('a78', 'Atari 7800', '.a78')]:
    build_emu(app, CORE, SRCS, dict(id='emu.' + app, name=name, root='/sd/roms/' + app, exts=exts,
                                    save_dir='/sd/odroid/data/' + app, run='prosystem_run'),
              defines=DEFS, includes=INCS, opt='-O2', cflags=[])
