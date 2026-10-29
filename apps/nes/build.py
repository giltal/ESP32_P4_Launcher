#!/usr/bin/env python3
"""
Build the NES app (nofrendo) as a .papp:

  python apps/nes/build.py          -> build/apps/nes/nes.papp

nofrendo is the RetroESP32_P4 copy (LGPL) with its ESP32 layer replaced by
osd_papp.c + main.c (see nes_bridge.h).
"""

import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

NOF = os.path.join(HERE, "nofrendo")
CORE = ["bitmap.c", "config.c", "event.c", "gui.c", "gui_elem.c", "intro.c", "log.c", "memguard.c",
        "nofrendo.c", "pcx.c", "vid_drv.c", "cpu/dis6502.c", "cpu/nes6502.c", "libsnss/libsnss.c",
        "nes/mmclist.c", "nes/nes.c", "nes/nes_mmc.c", "nes/nes_pal.c", "nes/nes_ppu.c", "nes/nes_rom.c",
        "nes/nesinput.c", "nes/nesstate.c", "sndhrdw/fds_snd.c", "sndhrdw/mmc5_snd.c", "sndhrdw/nes_apu.c",
        "sndhrdw/vrcvisnd.c"]

# Legacy C: keep the build output readable
QUIET = ["-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function", "-Wno-unused-value",
         "-Wno-pointer-sign", "-Wno-char-subscripts", "-Wno-missing-braces", "-Wno-switch",
         "-Wno-discarded-qualifiers", "-Wno-misleading-indentation", "-Wno-format", "-Wno-maybe-uninitialized",
         "-Wno-stringop-truncation", "-Wno-dangling-else", "-Wno-parentheses", "-Wno-address",
         "-Wno-int-conversion", "-Wno-incompatible-pointer-types", "-Wno-implicit-function-declaration"]


def main():
    sources = [os.path.join(HERE, "main.c"), os.path.join(HERE, "osd_papp.c")]
    sources += [os.path.join(NOF, c) for c in CORE]
    sources += sorted(glob.glob(os.path.join(NOF, "mappers", "*.c")))
    includes = [HERE, os.path.join(HERE, "compat"), os.path.join(NOF, "include"), NOF]
    papp_build.build("nes", sources, includes=includes, libc=True,
                     extra_cflags=QUIET + ["-O2"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
