#!/usr/bin/env python3
"""Controller test (Store, Tools): every axis, hat and button of a controller, motion, rumble.
    python apps/controller_test/build.py  -> build/apps/controller_test/controller_test.papp
Draws with the education apps' edu_ui (apps/education/common/edu_ui.c), linked in."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

COMMON = os.path.join(ROOT, "apps", "education", "common")
papp_build.build("controller_test", [os.path.join(HERE, "main.c"), os.path.join(COMMON, "edu_ui.c")],
                 includes=[COMMON], libc=True)
