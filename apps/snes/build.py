#!/usr/bin/env python3
"""SNES app (snes9x + snes_run.c, from RetroESP32_P4; no SuperFX).
    python apps/snes/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core", "snes9x")
SRCS = ["spc700.c", "memmap.c", "apu.c", "globals.c", "c4emu.c", "cpuops.c", "dsp1emu.c", "srtc.c",
        "apu_blargg.c", "clip.c", "cpu.c", "gfx.c", "soundux.c", "cpuexec.c", "obc1.c", "dsp2emu.c",
        "tile.c", "ppu.c", "dma.c", "save.c", "c4.c", "dsp1.c", "getset.c", "fxstub.c",
        os.path.join("..", "..", "snes_run.c")]

build_emu("snes", CORE, SRCS,
          dict(id="emu.snes", name="SNES", root="/sd/roms/snes", exts=".smc,.sfc",
               save_dir="/sd/odroid/data/snes", run="snes_run", noext=True, stack=65536),
          defines=["NO_ZERO_LUT"], includes=[".", "..", HERE], opt="-O3")
