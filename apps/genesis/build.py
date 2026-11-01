#!/usr/bin/env python3
"""Sega Genesis / Mega Drive app (gwenesis + genesis_run.c, from RetroESP32_P4).
    python apps/genesis/build.py"""
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
DIRS = ["src/bus", "src/cpus/M68K", "src/cpus/Z80", "src/io", "src/savestate", "src/sound", "src/vdp"]
SRCS = [os.path.relpath(p, CORE) for d in DIRS for p in sorted(glob.glob(os.path.join(CORE, d, "*.c")))]
SRCS.append(os.path.join("..", "genesis_run.c"))

build_emu("genesis", CORE, SRCS,
          dict(id="emu.genesis", name="Genesis", root="/sd/roms/gen", exts=".md,.gen,.bin",
               save_dir="/sd/odroid/data/genesis", run="genesis_run", noext=True, stack=49152),
          defines=["GWENESIS_AUDIO_ACCURATE=0"], includes=DIRS + [HERE], opt="-O3",
          cflags=["-include", "stdint.h"])
