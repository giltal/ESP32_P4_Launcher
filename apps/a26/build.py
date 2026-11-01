#!/usr/bin/env python3
"""Atari 2600 app (stella, from RetroESP32_P4).
    python apps/a26/build.py"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "common", "odroid"))
from emubuild import build_emu  # noqa: E402

CORE = os.path.join(HERE, "core")
SRCS = ['stella_run.cpp', 'common/Base.cpp', 'common/MouseControl.cpp', 'common/SoundSDL.cpp', 'emucore/Booster.cpp', 'emucore/Cart.cpp', 'emucore/Cart2K.cpp', 'emucore/Cart3E.cpp', 'emucore/Cart3F.cpp', 'emucore/Cart4K.cpp', 'emucore/CartAR.cpp', 'emucore/CartCV.cpp', 'emucore/CartDPC.cpp', 'emucore/CartE0.cpp', 'emucore/CartE7.cpp', 'emucore/CartF4.cpp', 'emucore/CartF4SC.cpp', 'emucore/CartF6.cpp', 'emucore/CartF6SC.cpp', 'emucore/CartF8.cpp', 'emucore/CartF8SC.cpp', 'emucore/CartFA.cpp', 'emucore/CartFE.cpp', 'emucore/CartUA.cpp', 'emucore/Console.cpp', 'emucore/Control.cpp', 'emucore/Driving.cpp', 'emucore/Genesis.cpp', 'emucore/Joystick.cpp', 'emucore/Keyboard.cpp', 'emucore/M6502.cpp', 'emucore/M6532.cpp', 'emucore/MD5.cpp', 'emucore/NullDev.cpp', 'emucore/Paddles.cpp', 'emucore/Props.cpp', 'emucore/PropsSet.cpp', 'emucore/Random.cpp', 'emucore/Serializer.cpp', 'emucore/Settings.cpp', 'emucore/StateManager.cpp', 'emucore/Stubs.cpp', 'emucore/Switches.cpp', 'emucore/System.cpp', 'emucore/TIA.cpp', 'emucore/TIASnd.cpp', 'emucore/TIATables.cpp', 'emucore/TrackBall.cpp']
DEFS = ['SOUND_SUPPORT=1', 'JUMP_TABLES_IN_TEXT_SECTION=1']

build_emu('a26', CORE, SRCS, dict(id="emu.a26", name='Atari 2600', root="/sd/roms/a26", exts='.a26,.bin',
                                 save_dir="/sd/odroid/data/a26", run='stella_run'),
          defines=DEFS, includes=['.', 'common', 'emucore', 'gui'], opt='-O2', cxx=True)
