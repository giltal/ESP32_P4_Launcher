#!/usr/bin/env python3
"""PC Engine app (huexpress, from RetroESP32_P4).
    python apps/pce/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['engine/bios.c', 'engine/bp.c', 'engine/cd.c', 'engine/cheat.c', 'engine/debug.c', 'engine/dis.c', 'engine/edit_ram.c', 'engine/followop.c', 'engine/format.c', 'engine/gfx.c', 'engine/h6280.c', 'engine/hard_pce.c', 'engine/hcd.c', 'engine/lsmp3.c', 'engine/mix.c', 'engine/ogglength.c', 'engine/optable.c', 'engine/pce.c', 'engine/pcecd.c', 'engine/romdb.c', 'engine/sound.c', 'engine/sprite.c', 'engine/subs_eagle.c', 'engine/trans_fx.c', 'iniconfig.c', 'utils.c', 'osd_dummy_cd.c', 'huexpress_run.c']
DEFS = ['LSB_FIRST=1']
INCS = ['.', 'includes', 'engine', 'include']

for app, name, exts in [('pce', 'PC Engine', '.pce')]:
    build_emu(app, CORE, SRCS, dict(id='emu.' + app, name=name, root='/sd/roms/' + app, exts=exts,
                                    save_dir='/sd/odroid/data/' + app, run='huexpress_run'),
              defines=DEFS, includes=INCS, opt='-O2', cflags=['-fgnu89-inline'])
