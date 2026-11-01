#!/usr/bin/env python3
"""Game Boy and Game Boy Color apps (gnuboy, from RetroESP32_P4):
    python apps/gb/build.py      -> build/apps/gb/gb.papp, build/apps/gbc/gbc.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ["cpu.c", "debug.c", "emu.c", "events.c", "exports.c", "fastmem.c", "hw.c", "inflate.c",
        "keytable.c", "lcd.c", "lcdc.c", "loader.c", "main.c", "mem.c", "newsound.c", "palette.c",
        "path.c", "rccmds.c", "rcfile.c", "rckeys.c", "rcvars.c", "refresh.c", "rtc.c", "save.c",
        "sound.c", "split.c", "gnuboy_run.c"]
DEFS = ["GNUBOY_NO_MINIZIP=1", "IS_LITTLE_ENDIAN=1", "vid_init=gnuboy_vid_init", "cpu=gnuboy_cpu",
        "rom_load=gnuboy_rom_load"]

for app, name, exts in (("gb", "Game Boy", ".gb"), ("gbc", "Game Boy Color", ".gbc")):
    build_emu(app, CORE, SRCS, dict(id="emu." + app, name=name, root="/sd/roms/" + app, exts=exts,
                                    save_dir="/sd/odroid/data/" + app, run="gnuboy_run"),
              defines=DEFS, includes=["include"])
