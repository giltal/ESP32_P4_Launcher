#!/usr/bin/env python3
"""
Prepares a launcher release for Settings -> Update: copies each board's built image
and writes latest.json next to them, then prints the gh command that publishes them.

    python tools/make_release.py [--out build/release]

Every board under boards/ (but the RETIRED ones) whose image is built at the current version goes in:
guition_p4_43 from launcher/build/launcher.bin, any other <id> from
launcher/build_<id>/launcher_<id>.bin (idf.py -B build_<id> -DLX_BOARD=<id> build).

Each board also gets launcher_<id>_full.bin: bootloader, partition table and launcher in one
file for a first install at address 0 (esptool or the ESP Tool web page).

latest.json lists each board under "boards"; its top-level fields are guition_p4_43's,
which is where launchers from before per-board releases look. "boards_big" lists them again for launchers
0.38+ on the 6.5 MB slots (components/lx_update/lx_update_net.c).

Images over 3 MB: launchers before 0.38 (3 MB slots) can't install them, so such a board's "boards" entry
(and the top level) must name a bridge release, <= 3 MB and 0.38+, which moves the board to the bigger
slots; after that it reads "boards_big". Pass the bridge release's latest.json:
    gh release download v0.38.0 -R giltal/ESP32_P4_Launcher -p latest.json -O build/bridge_latest.json
    python tools/make_release.py --bridge build/bridge_latest.json The board reads
releases/latest/download/latest.json, so the newest published release (not a draft
or pre-release) is the one devices install.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "giltal/ESP32_P4_Launcher"
FIRST = "guition_p4_43"
# Retired boards (docs/BOARD_SUPPORT.md) and their last release; later releases leave them out, and
# Settings > Update on them says there is no release for the board
RETIRED = {"waveshare_p4_rpi_dsi": "0.30.0"}
# Launchers before 0.38 have 3 MB update slots (old partition table); 0.38+ moves a board to 6.5 MB slots
# on its first start (components/lx_update/lx_layout.c). A bigger image can't reach a board still on the
# old table, so for it "boards" names a bridge release (--bridge) and "boards_big" the image itself.
OLD_SLOT = 0x300000
FIRST_BIG = "0.38.0"             # the first launcher that moves itself and reads "boards_big"


def vtuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v))


def image_of(board):
    name = "launcher.bin" if board == FIRST else f"launcher_{board}.bin"
    folder = "build" if board == FIRST else f"build_{board}"
    return os.path.join(ROOT, "launcher", folder, name), name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    ap.add_argument("--bridge", metavar="LATEST_JSON",
                    help="latest.json of a release <= 3 MB, 0.38+: what boards on the old 3 MB slots get "
                         "when this release's image is bigger")
    args = ap.parse_args()

    header = open(os.path.join(ROOT, "components", "lx_common", "include", "lx_common.h")).read()
    version = re.search(r'LX_LAUNCHER_VERSION\s+"([^"]+)"', header).group(1)
    tag = f"v{version}"
    os.makedirs(args.out, exist_ok=True)

    bridge = None
    if args.bridge:
        bridge = json.load(open(args.bridge))
        if vtuple(bridge["version"]) < vtuple(FIRST_BIG):
            raise SystemExit(f"--bridge: {bridge['version']} is older than {FIRST_BIG}, it can't move a board")
    boards, big, files = {}, {}, []
    for board in sorted(os.listdir(os.path.join(ROOT, "boards"))):
        if board in RETIRED and vtuple(version) > vtuple(RETIRED[board]):
            print(f"skip {board}: retired after {RETIRED[board]}")
            continue
        path, name = image_of(board)
        if not os.path.isfile(path):
            print(f"skip {board}: {os.path.relpath(path, ROOT)} not built")
            continue
        data = open(path, "rb").read()
        if version.encode() not in data:
            print(f"skip {board}: image is not version {version}, rebuild it")
            continue
        shutil.copyfile(path, os.path.join(args.out, name))
        files.append(name)
        full = f"launcher_{board}_full.bin"
        # flash_args = the build's own addresses and files (bootloader, partition table, otadata, app)
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32p4", "merge_bin",
                        "-o", os.path.abspath(os.path.join(args.out, full)), "@flash_args"],
                       cwd=os.path.dirname(path), check=True, stdout=subprocess.DEVNULL)
        files.append(full)
        boards[board] = {
            "version": version,
            "url": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        # Wi-Fi chip firmware matching this board's esp_hosted (tools/build_c6_fw.py), offered by Settings > Update
        hosted = re.search(r'espressif/esp_hosted:\s*"?([0-9.]+)',
                           open(os.path.join(ROOT, "boards", board, "board", "idf_component.yml")).read())
        c6 = os.path.join(ROOT, "build", "c6_fw", f"c6_{hosted.group(1)}.bin") if hosted else ""
        # build_c6_fw.py builds the SDIO image; a board wired over SPI would lose its Wi-Fi chip to it
        spi = "CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE=y" in open(
            os.path.join(ROOT, "boards", board, "sdkconfig.board")).read()
        if spi:
            print(f"note: {board}: C6 over SPI, no C6 firmware in the release for it")
        elif c6 and os.path.isfile(c6):
            c6_name = os.path.basename(c6)
            if c6_name not in files:
                shutil.copyfile(c6, os.path.join(args.out, c6_name))
                files.append(c6_name)
            c6_data = open(c6, "rb").read()
            boards[board]["c6"] = {
                "version": hosted.group(1),
                "url": f"https://github.com/{REPO}/releases/download/{tag}/{c6_name}",
                "size": len(c6_data),
                "sha256": hashlib.sha256(c6_data).hexdigest(),
            }
        elif hosted and hosted.group(1).startswith("2."):
            print(f"note: {board}: no {os.path.relpath(c6, ROOT)} (python tools/build_c6_fw.py --board {board})")
    if not boards:
        raise SystemExit("no board image built at this version")
    # 0.38+ on the big slots: this release. Boards still on the 3 MB slots: this release if it fits, else the bridge
    big = {b: dict(e) for b, e in boards.items()}
    for board, e in list(boards.items()):
        if e["size"] <= OLD_SLOT:
            continue
        old = (bridge or {}).get("boards", {}).get(board)
        if not old or old.get("size", OLD_SLOT + 1) > OLD_SLOT:
            raise SystemExit(f"{board}: image {e['size']:#x} > {OLD_SLOT:#x}: boards still on the old 3 MB slots "
                             f"need a bridge release (--bridge <its latest.json> with a <= 3 MB {board} image)")
        # (its own c6 entry stays: the Wi-Fi chip firmware that matches the bridge launcher)
        boards[board] = {**old, "version": old.get("version", bridge["version"])}
        print(f"{board}: {e['size']:#x} > 3 MB, old-layout boards get the bridge {boards[board]['version']}")
    if FIRST not in boards:
        print(f"warning: no {FIRST} image; launchers older than per-board releases will see no update")

    manifest = {"version": version, **boards.get(FIRST, {}), "boards": boards, "boards_big": big}
    if FIRST in boards:
        manifest["version"] = boards[FIRST]["version"]       # what launchers before per-board releases compare
    with open(os.path.join(args.out, "latest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    files.append("latest.json")
    print(json.dumps(manifest, indent=2))
    assets = " ".join(f'"{args.out}/{n}"' for n in files)
    print(f"\ngh release create {tag} -R {REPO} --title \"Launcher {version}\" {assets}")


if __name__ == "__main__":
    main()
