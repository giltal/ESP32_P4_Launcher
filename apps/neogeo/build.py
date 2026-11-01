#!/usr/bin/env python3
"""Neo Geo app (gngeo, from RetroESP32_P4):
    python apps/neogeo/build.py      -> build/apps/neogeo/neogeo.papp
ROMs: /sd/roms/neogeo/<game>.zip plus neogeo.zip (BIOS). Large games stream
sprites from a .ctile cache the first run builds next to the ROM."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
G68K = ["generator68k/cpu68k.c"] + [f"generator68k/cpu68k-{c}.c" for c in "0123456789abcdef"] + \
       ["generator68k/reg68k.c"]
SRCS = ["emu.c", "memory.c", "video.c", "timer.c", "pd4990a.c", "frame_skip.c", "neoboot.c", "roms.c",
        "neocrypt.c", "unzip.c", "stb_zlib.c", "stb_image.c", "resfile.c", "mame_layer.c", "state.c", "list.c",
        "gnutil.c", "transpack.c", "esp32_platform.c", "adpcm_cache.c", "generator68k_interf.c",
        "mamez80/z80.c", "mamez80_interf.c", "ym2610/ym2610.c", "ym2610/2610intf.c", "neogeo_run.c"] + G68K
DEFS = ["USE_MAMEZ80=1", "HAVE_BASENAME=1", "ESP32_PLATFORM=1", "EMBEDDED_FS=1", 'ROOTPATH="/sd/roms/neogeo/"',
        "HAVE_LIBZ=1", "USE_GENERATOR68K=1"]
HOT = {f: ["-O3"] for f in G68K + ["memory.c"]}           # the 68K core, as in the reference

build_emu("neogeo", CORE, SRCS, dict(id="emu.neogeo", name="Neo Geo", root="/sd/roms/neogeo", exts=".zip",
                                     save_dir="/sd/roms/neogeo/save", run="neogeo_run", stack=49152,
                                     exclude="neogeo.zip,gngeo_data.zip", nodirs=True),
          defines=DEFS, includes=["include", ".", "generator68k", "mamez80", "ym2610"],
          cflags=["-Wno-old-style-definition", "-Wno-shift-count-overflow", "-include", "stdio.h", "-include", "stdarg.h"], per_file=HOT)
