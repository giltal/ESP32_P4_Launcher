# ESP32-P4 Launcher — Project Context

Launcher firmware for ESP32-P4 boards that loads **relocatable PAPP v2 apps** from the SD card into
PSRAM and runs them with a DLL-style interface layer. Successor to the PAPP concept in
`C:\ESPIDFprojects\RetroESP32_P4` (reference project: drivers, ports, history).

Design: `docs/ARCHITECTURE.md`. Plan + status: `docs/WORKPLAN.md`.

## Layout

```
launcher/                 ESP-IDF project (factory app; main.c + home.c = LVGL home screen). Board: -DLX_BOARD=<id> (default guition_p4_43)
boards/<id>/board/        BSP implementation (component "board"); boards/<id>/sdkconfig.board
components/bsp/           BSP contract (bsp.h) — the core only talks to hardware through it
components/papp_abi/      papp_abi.h — .papp v2 format + interfaces (shared with apps; no IDF includes)
components/lx_loader/     App Manager: loader, relocation, imports, services (SYS/MEM/FS/DISP/AUDI/INPT/THRD/UI), teardown
components/lx_devlink/    DevLink: framed protocol over USB-Serial-JTAG (devlink_proto.h)
components/lx_common/     crc32, launcher version
components/lx_gfx/        software drawing + 8x16 font for launcher screens (rotation-aware)
components/lx_comp/       compositor: app surfaces → PPA scale/rotate → panel; launcher screens draw via it
components/lx_audio/      audio engine: per-stream rings → resample → mix → BSP codec (fixed rate)
components/lx_ui/         launcher-drawn screens for apps: content browser (pick_file) + message box
components/lx_input/      input manager: USB HID gamepads/keyboards + board buttons (merged into P1) → players + event queue; HOME chord
papp-sdk/                 App SDK: include/papp/papp.h, runtime/ (crt0, libc glue), ld/papp.ld,
                          tools/papp_build.py, tools/papp_pack.py
apps/                     Test/demo apps (hello, libc_test, gfx_demo: DISP test, args w h mode secs bufs;
                          audio_test: AUDI test, args mix|cycles|block; input_test: INPT event log, arg secs;
                          thread_test: THRD test (--libc), args all|leave|exit|spin;
                          fast_bench: PSRAM vs FAST (internal SRAM) placement benchmark, arg rounds;
                          browse_test: UI browser test, args root exts save_dir picks — all test apps are
                          PAPP_MF_HIDDEN: shown on the home screen only with Settings → Show developer apps)
apps/<emu>/               Emulators, cores copied from RetroESP32_P4, each with a build.py:
                          nes (own glue), gb+gbc, sms+gg+col, pce, a78, spectrum, genesis, snes, lynx (C++),
                          a26 (C++), a800 (C++), neogeo. No args = ROM browser, or `-- <rom> [resume]`.
                          `python apps/build_all.py [name ...]` builds every app with a build.py
apps/common/odroid/       RetroESP32 odroid_* / app_common / FreeRTOS / ESP-IDF shims on the SDK, so the cores
                          and their *_run.c build unchanged; emu_main.c (browser → EMU_RUN; Exit game → PAPP_EXIT_RELAUNCH = back to the browser), emubuild.py;
                          X = in-game menu, Y = volume, unless the core sets odroid_input_xy_menu_disable
                          (SNES, Genesis, Neo Geo): then touch the left (menu) / right (volume) screen edge
apps/common/icons/        88x88 icons of the emulator apps (emubuild copies <app>.png)
apps/common/v1compat/     v1 `app_services_t` host on the SDK + v1build.py: the RetroESP32 v1 PSRAM games
                          doom, opentyrian, quake, duke3d build with their own glue unchanged; frames go to the PPA without a copy (DISP v3 `present_ext`)
tools/papp_dev.py         DevLink host tool
tests/host/test_reloc.py  Host relocation equivalence test
```

## Environment

- ESP-IDF v5.5.2 at `C:\Users\97254\esp\v5.5.2\esp-idf`; in PowerShell: `. .\tools\idf_env.ps1`
- Python with pyserial + pyelftools: `C:\Users\97254\.espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe`
- Board: Guition ESP32-P4 4.3", chip rev v1.x (needs `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`),
  USB-Serial-JTAG on **COM9**. UART0 is the text console; USB-Serial-JTAG belongs to DevLink.

## Dev loop (works without touching the board)

```powershell
cd launcher; idf.py -p COM9 build flash; cd ..                     # launcher (only when it changes)
python papp-sdk\tools\papp_build.py --name hello --src apps\hello\main.c [--libc]
python tools\papp_dev.py --port COM9 deploy build\apps\hello\hello.papp --timeout 20 -- arg1
python tools\papp_dev.py --port COM9 crash --elf build\apps\hello\hello.elf    # after a panic reboot
python tests\host\test_reloc.py                                               # packer/relocator check
```

Apps in `/sd/apps/<dir>/<file>.papp` appear on the home screen. Icon: an `icon.png` in the app's source
folder (PNG, square, best 88x88, transparency OK; corners are rounded by the launcher) is copied by
`papp_build.py` and uploaded by `deploy` to `/sd/apps/<dir>/icon.png`; without one the tile shows the initial. Every `papp_dev.py`
command also sets the board clock from the PC.
`deploy` = upload to `/sd/apps/<name>/<name>.papp` → run → stream logs → exit code of the app
(124 timeout). `shot out.png` = screenshot of the visible framebuffer, in landscape (open it to see
the screen). Other commands: `info ls stat put get rm mkdir mv run stop status log reboot crash-clear`.
Input without touching the board: `input state [--watch 10]`, `input pad A+START [--hold ms]`,
`input key ENTER`, `input text "hi"`, or timed during a run: `deploy app.papp --inject 500:pad:A --inject 900:text:hi`.

## Rules

- Apps never include ESP-IDF headers; launcher core never includes board headers.
- New interface functions are **appended** and the interface version bumped; breaking change = new id.
- Every service entry point uses `SVC_ENTER/SVC_LEAVE` (forced stop waits for it) and tracks what it
  creates so teardown can reclaim it.
- `CONFIG_LX_APP_TEST_BASE_SHIFT_PAGES` must be 0 in normal builds (relocation test aid).
- `ESP_SYSTEM_PMP_IDRAM_SPLIT` stays off (FAST region needs executable heap SRAM). After copying an
  sdkconfig file back, touch it: an old timestamp makes the build silently keep the previous config.
- Commit/push only when asked.
