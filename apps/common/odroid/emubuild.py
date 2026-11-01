"""
Builds a RetroESP32_P4 emulator core + its run file as a PAPP app on the
odroid compatibility layer (odroid_papp.c, rtos_papp.c, compat/ shims) and
the shared emu_main.c (launcher browser -> run).

    build_emu("gb", core_dir, ["cpu.c", ...], app=dict(id="emu.gb", name="Game Boy",
              root="/sd/roms/gb", exts=".gb", save_dir="/sd/odroid/data/gb",
              run="gnuboy_run"), defines=["GNUBOY_NO_MINIZIP=1"], includes=["include"])

Several apps can share one core (GB / GBC, SMS / GG / Coleco): call it once
per app with a different `name` and app dict.
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "papp-sdk", "tools"))
import papp_build  # noqa: E402

QUIET = ["-Wno-format", "-Wno-unused-variable", "-Wno-unused-function", "-Wno-unused-but-set-variable",
         "-Wno-maybe-uninitialized", "-Wno-implicit-function-declaration", "-Wno-pointer-sign",
         "-Wno-int-conversion", "-Wno-incompatible-pointer-types", "-Wno-dangling-pointer",
         "-Wno-array-bounds", "-Wno-address", "-Wno-restrict", "-Wno-builtin-declaration-mismatch",
         "-Wno-char-subscripts", "-Wno-sign-compare", "-Wno-parentheses", "-Wno-missing-braces",
         "-Wno-misleading-indentation", "-Wno-unused-value", "-Wno-stringop-truncation",
         "-Wno-stringop-overflow", "-Wno-format-overflow", "-Wno-return-type", "-Wno-switch",
         "-Wno-dangling-else", "-Wno-unused-label", "-Wno-discarded-qualifiers",
         "-Wno-enum-conversion", "-Wno-overflow", "-Wno-shift-negative-value", "-Wno-attributes",
         "-Wno-format-truncation", "-Wno-unused-const-variable", "-Wno-nonnull", "-Wno-use-after-free",
         "-Wno-comment", "-Wno-unknown-pragmas", "-Wno-implicit-fallthrough", "-Wno-narrowing",
         "-Wno-uninitialized", "-Wno-strict-aliasing", "-Wno-int-to-pointer-cast",
         "-Wno-pointer-to-int-cast", "-Wno-unused-local-typedefs", "-Wno-class-memaccess",
         "-Wno-deprecated-declarations", "-Wno-volatile", "-Wno-register", "-Wno-write-strings",
         "-Wno-sequence-point", "-Wno-aggressive-loop-optimizations", "-Wno-type-limits",
         "-Wno-logical-not-parentheses", "-Wno-bool-operation", "-Wno-unused-result",
         "-Wno-implicit-int", "-Wno-return-mismatch"]

SHIM = [os.path.join(HERE, "odroid_papp.c"), os.path.join(HERE, "rtos_papp.c")]


def build_emu(name, core_dir, srcs, app, defines=(), includes=(), opt="-O2", cflags=(), cxx=False,
              icon=None, per_file=None):
    """srcs are relative to core_dir. app: id, name, root, exts, save_dir, run, [noext], [stack],
    [exclude] (files the browser hides), [nodirs] (no sub-folders).
    per_file: {src relative to core_dir: [extra flags]} (e.g. -O3 for hot files)."""
    main = os.path.join(HERE, "emu_main.c")
    q = lambda s: '"' + s + '"'
    main_defs = [f"-DEMU_ID={q(app['id'])}", f"-DEMU_NAME={q(app['name'])}", f"-DEMU_ROOT={q(app['root'])}",
                 f"-DEMU_EXTS={q(app['exts'])}", f"-DEMU_SAVE_DIR={q(app['save_dir'])}",
                 f"-DEMU_RUN={app['run']}", f"-DEMU_SAVE_NOEXT={1 if app.get('noext') else 0}",
                 f"-DEMU_STACK={app.get('stack', 32768)}", f"-DEMU_NO_DIRS={1 if app.get('nodirs') else 0}"] + \
                ([f"-DEMU_EXCLUDE={q(app['exclude'])}"] if app.get("exclude") else [])
    sources = [main] + SHIM + ([os.path.join(HERE, "cxx_papp.c")] if cxx else []) + \
        [os.path.join(core_dir, s) for s in srcs]
    incs = [os.path.join(HERE, "compat"), os.path.join(HERE, "include")] + \
           [i if os.path.isabs(i) else os.path.join(core_dir, i) for i in includes]
    app_flags = QUIET + ["-D" + d for d in defines] + list(cflags) + [opt]
    if cxx:
        app_flags += ["-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics"]
    out = os.path.join(ROOT, "build", "apps", name)
    result = papp_build.build(name, sources, includes=incs, libc=True, app_cflags=app_flags,
                              per_file_cflags={main: main_defs, **{os.path.join(core_dir, f): list(v)
                                                                   for f, v in (per_file or {}).items()}},
                              out_dir=out,
                              extra_libs=(["-lstdc++", "-lsupc++"] if cxx else []),
                              icon_dir=os.path.dirname(icon) if icon else None)
    # default icon: apps/common/icons/<name>.png (88x88 PNG)
    default_icon = os.path.join(os.path.dirname(HERE), "icons", name + ".png")
    if not icon and os.path.isfile(default_icon):
        shutil.copyfile(default_icon, os.path.join(out, "icon.png"))
    return result
