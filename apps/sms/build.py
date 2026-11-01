#!/usr/bin/env python3
"""Sega Master System, Game Gear and ColecoVision apps (smsplus, from RetroESP32_P4).
    python apps/sms/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['config.c', 'crc32.c', 'error.c', 'fileio.c', 'hvc.c', 'loadrom.c', 'memz80.c', 'pio.c', 'render.c', 'render_bp_lut.c', 'render_lut.c', 'sms.c', 'state.c', 'system.c', 'tms.c', 'tms_bp_expand.c', 'tms_lookup.c', 'tms_mc_lookup.c', 'tms_obj_lut.c', 'tms_txt_lookup.c', 'vdp.c', 'cpu/z80.c', 'cpu/z80_SZHVC_add_table.c', 'cpu/z80_SZHVC_sub_table.c', 'sound/emu2413.c', 'sound/fmintf.c', 'sound/sn76489.c', 'sound/sound.c', 'sound/ym2413.c', 'smsplus_run.c']
DEFS = ['snd=smsplus_snd', 'sound_reset=smsplus_sound_reset']
INCS = ['include', '.', 'cpu', 'sound']

for app, name, exts in [('sms', 'Master System', '.sms'), ('gg', 'Game Gear', '.gg'), ('col', 'ColecoVision', '.col')]:
    build_emu(app, CORE, SRCS, dict(id='emu.' + app, name=name, root='/sd/roms/' + app, exts=exts,
                                    save_dir='/sd/odroid/data/' + app, run='smsplus_run',
                                    exclude='BIOS.col' if app == 'col' else None),
              defines=DEFS, includes=INCS, opt='-O2', cflags=[])
