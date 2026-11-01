#!/usr/bin/env python3
"""Builds every app that has a build.py (emulators and games):
    python apps/build_all.py [name ...]      -> build/apps/<app>/<app>.papp (+ icon.png)
Prints one line per build script and a summary; exit code 1 if any failed."""
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

only = set(sys.argv[1:])
scripts = sorted(d for d in os.listdir(HERE)
                 if os.path.isfile(os.path.join(HERE, d, "build.py")) and (not only or d in only))
failed = []
for d in scripts:
    t = time.time()
    r = subprocess.run([sys.executable, os.path.join(HERE, d, "build.py")], capture_output=True, text=True)
    built = [ln.split(":")[0] for ln in r.stdout.splitlines() if ".papp (" in ln]
    status = "ok" if r.returncode == 0 else "FAILED"
    print(f"{d:12s} {status:6s} {time.time() - t:5.0f}s  {' '.join(built)}", flush=True)
    if r.returncode:
        failed.append(d)
        print("\n".join((r.stdout + r.stderr).splitlines()[-15:]))
print(f"{len(scripts) - len(failed)}/{len(scripts)} built" + (f"; failed: {' '.join(failed)}" if failed else ""))
sys.exit(1 if failed else 0)
