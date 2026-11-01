#!/usr/bin/env python3
"""ZX Spectrum app (spectemu, from RetroESP32_P4).
    python apps/spectrum/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['ay_sound.c', 'compr.c', 'interf.c', 'keynames.c', 'loadim.c', 'misc.c', 'rom_imag.c', 'snapshot.c', 'spconf.c', 'spect.c', 'spectrum_run.c', 'spkey.c', 'spmain.c', 'spperif.c', 'spscr.c', 'spsound.c', 'sptape.c', 'sptiming.c', 'stubs.c', 'tapefile.c', 'vgakey.c', 'vgascr.c', 'z80.c', 'z80optab.c', 'z80_op1.c', 'z80_op2.c', 'z80_op3.c', 'z80_op4.c', 'z80_op5.c', 'z80_op6.c', 'z80_step.c']
DEFS = ['SPECT_MEM=1', 'Z80C=1', 'HAVE_SOUND=1']
INCS = ['include', '.']

for app, name, exts in [('spectrum', 'ZX Spectrum', '.z80,.sna')]:
    build_emu(app, CORE, SRCS, dict(id='emu.' + app, name=name, root='/sd/roms/' + app, exts=exts,
                                    save_dir='/sd/odroid/data/' + app, run='spectrum_run'),
              defines=DEFS, includes=INCS, opt='-O3', cflags=[])
