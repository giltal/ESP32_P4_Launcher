#!/usr/bin/env python3
"""HTTP interface test (hidden), two builds:
    python apps/http_test/build.py  -> build/apps/http_test/http_test.papp (PAPP_MF_WIFI)
                                       build/apps/http_test_noflag/http_test_noflag.papp (without)"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "papp-sdk", "tools"))
import papp_build  # noqa: E402

src = [os.path.join(HERE, "main.c")]
papp_build.build("http_test", src, libc=True)
papp_build.build("http_test_noflag", src, libc=True, app_cflags=["-DHTTP_TEST_NOFLAG"], version_dir=HERE)
