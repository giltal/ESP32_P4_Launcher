#!/usr/bin/env python3
"""
papp_build.py — compile, link and pack a PAPP app with the bare riscv32-esp-elf toolchain.

  python papp-sdk/tools/papp_build.py --name hello --src apps/hello/main.c
  python papp-sdk/tools/papp_build.py --name libc_test --src apps/libc_test/main.c --libc

Output: build/apps/<name>/<name>.papp (+ .elf, .map). No ESP-IDF project needed.
(Interim driver until the CMake toolchain of WORKPLAN Phase 4.2.)
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys

SDK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(SDK)
sys.path.insert(0, os.path.join(SDK, "tools"))
import papp_pack  # noqa: E402

ARCH = ["-march=rv32imafc_zicsr_zifencei", "-mabi=ilp32f"]
CFLAGS = ARCH + ["-mcmodel=medany", "-Os", "-g", "-ffunction-sections", "-fdata-sections",
                 "-fno-common", "-Wall", "-I" + os.path.join(SDK, "include"),
                 "-I" + os.path.join(ROOT, "components", "papp_abi", "include")]
WRAPS = ["malloc", "free", "calloc", "realloc", "memalign",
         "_malloc_r", "_free_r", "_calloc_r", "_realloc_r", "_memalign_r",
         "__retarget_lock_init", "__retarget_lock_init_recursive", "__retarget_lock_close",
         "__retarget_lock_close_recursive", "__retarget_lock_acquire",
         "__retarget_lock_acquire_recursive", "__retarget_lock_try_acquire",
         "__retarget_lock_try_acquire_recursive", "__retarget_lock_release",
         "__retarget_lock_release_recursive"]


def find_tool(name):
    exe = shutil.which(name)
    if exe:
        return exe
    pattern = os.path.join(os.path.expanduser("~"), ".espressif", "tools", "riscv32-esp-elf", "*",
                           "riscv32-esp-elf", "bin", name + (".exe" if os.name == "nt" else ""))
    hits = sorted(glob.glob(pattern))
    if not hits:
        sys.exit(f"papp_build: {name} not found (install ESP-IDF tools or add the toolchain to PATH)")
    return hits[-1]


def run(cmd):
    r = subprocess.run(cmd)
    if r.returncode:
        sys.exit(f"papp_build: command failed: {' '.join(cmd[:3])} ...")


def build(name, sources, includes=(), libc=False, out_dir=None, link_base=0, extra_cflags=(),
          fast_link_base=0):
    gcc = find_tool("riscv32-esp-elf-gcc")
    out_dir = out_dir or os.path.join(ROOT, "build", "apps", name)
    obj_dir = os.path.join(out_dir, "obj")
    os.makedirs(obj_dir, exist_ok=True)

    rt = os.path.join(SDK, "runtime")
    runtime = [os.path.join(rt, "crt0.c"), os.path.join(rt, "papp_api.c")]
    runtime.append(os.path.join(rt, "libc_glue.c" if libc else "mini_libc.c"))

    cflags = CFLAGS + ["-I" + i for i in includes] + list(extra_cflags)
    if not libc:
        cflags += ["-ffreestanding", "-fno-tree-loop-distribute-patterns"]

    objs = []
    for src in runtime + list(sources):
        obj = os.path.join(obj_dir, os.path.splitext(os.path.basename(src))[0] + ".o")
        run([gcc] + cflags + ["-c", src, "-o", obj])
        objs.append(obj)

    ld_script = os.path.join(SDK, "ld", "papp.ld")
    if link_base or fast_link_base:
        text = open(ld_script, encoding="utf-8").read().replace(". = 0;", f". = {link_base:#x};", 1)
        if fast_link_base:
            text = text.replace(".fast 0x10000000 :", f".fast {fast_link_base:#x} :", 1)
        ld_script = os.path.join(out_dir, "papp_base.ld")
        open(ld_script, "w", encoding="utf-8").write(text)

    elf = os.path.join(out_dir, name + ".elf")
    ldflags = ARCH + ["-nostartfiles", "-T", ld_script, "-Wl,--gc-sections", "-Wl,--no-relax",
                      "-Wl,-q", "-Wl,--no-warn-rwx-segments", "-Wl,-Map=" + elf[:-4] + ".map"]
    if libc:
        ldflags += ["-nodefaultlibs"] + ["-Wl,--wrap=" + w for w in WRAPS]
        libs = ["-Wl,--start-group", "-lc", "-lm", "-lgcc", "-Wl,--end-group"]
    else:
        ldflags += ["-nostdlib"]
        libs = ["-lgcc"]
    run([gcc] + ldflags + ["-o", elf] + objs + libs)

    if link_base or fast_link_base:
        return elf, None
    papp = os.path.join(out_dir, name + ".papp")
    try:
        img = papp_pack.build_image(elf)
        open(papp, "wb").write(papp_pack.serialize(img))
    except papp_pack.PackError as e:
        sys.exit(f"papp_pack: error: {e}")
    print(f"{name}: {papp} (image {len(img.image):,} B, bss {img.bss_size:,} B, "
          f"{len(img.relocs)} relocs, {img.imports_count} imports"
          + (f", FAST {len(img.fast):,} B + bss {img.fast_bss_size:,} B" if img.fast or img.fast_bss_size else "")
          + ")")
    return elf, papp


def main():
    ap = argparse.ArgumentParser(description="Build a PAPP app")
    ap.add_argument("--name", required=True)
    ap.add_argument("--src", nargs="+", required=True)
    ap.add_argument("--inc", nargs="*", default=[])
    ap.add_argument("--libc", action="store_true", help="link newlib (stdio, malloc, math)")
    ap.add_argument("--out")
    ap.add_argument("--link-base", type=lambda s: int(s, 0), default=0,
                    help="link at this address instead of 0 (tests only, no .papp)")
    args = ap.parse_args()
    build(args.name, args.src, args.inc, args.libc, args.out, args.link_base)


if __name__ == "__main__":
    main()
