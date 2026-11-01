#!/usr/bin/env python3
"""Files app.
    python apps/files/build.py          -> build/apps/files/files.papp"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

papp_build.build("files", [os.path.join(HERE, "main.c")], libc=True)
