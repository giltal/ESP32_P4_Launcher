# ESP32-P4 Launcher — Work Plan

> Companion to [`ARCHITECTURE.md`](ARCHITECTURE.md). Draft v0.1, 2026-10-29.
> Estimates are in **person-weeks (pw)** for one developer familiar with the reference project.
> Target board for Phases 0–7: the reference Guition ESP32-P4 4.3″ (LCD), with the HDMI variant as a
> second board config. Toolchain: ESP-IDF v5.5.2, `riscv32-esp-elf-gcc`, Windows/PowerShell + Python.

---

## Status (2026-10-29, launcher 0.6.0)

| Item | State | Evidence |
|---|---|---|
| Repo skeleton, board package (SD only), launcher boots | done | flashed on Guition board, COM9 |
| **1.0 DevLink** (info/ls/stat/put/get/rm/mkdir/mv/run/stop/status/log/reboot/crash) + `papp_dev.py deploy` | done | 78 KB upload @ ~205 KB/s; logs + exit events streamed |
| **Spike A** relocatable `.papp` v2 (packer, loader, imports, manifest, CRC) | done | host test: byte-identical at 3 bases; on device apps run at `0x4A000000` and `0x4A030000` |
| SDK runtime (crt0, ctor/dtor, newlib glue, malloc → MEM) | first cut | `libc_test` passes on device (printf/float/qsort/heap/stdio files) |
| Teardown / forced stop (part of Spike C) | first cut | clean stop, forced stop of hung app, leaked blocks reclaimed; heap back to baseline every run |
| Crash reporting (core dump + app context + symbolization) | done | NULL store → `main.c:40` |
| BSP display: ST7701 MIPI-DSI, 2 framebuffers, tear-free flip, vsync, backlight | done | panel upright, colour order confirmed by eye |
| BSP touch: GT911 over shared I2C, polled, points in landscape coordinates | done | crosshair follows the finger (confirmed by eye) |
| BSP audio: ES8311 + I2S0 fixed 48 kHz 16-bit stereo, PA via codec | done | chime heard; 350 ms written in 290 ms (= minus 60 ms DMA queue) → clock correct |
| **2.3 Compositor** first cut (`lx_comp`): one RGB565 surface, FIT_ASPECT/INTEGER/STRETCH/NONE, one PPA SRM (scale + rotate) per present, tear-free flip, touch in surface coords; launcher screens go through it | first cut | `gfx_demo`: 320x240, 400x240, 256x224 (fit and integer), 160x100 stretch at 60 fps (PPA 3.6–6.1 ms); 800x480 1:1 at 23 fps (app drawing 28 ms + PPA 16 ms, serialised); orientation + letterbox checked by screenshot; heap at baseline after every run |
| **DISP v1** interface + SDK wrappers (`papp_disp_*`), optional import | done | apps built before DISP still load and run |
| **2.4 Audio engine** first cut (`lx_audio`): mixer task (core 1, 5 ms blocks) at the fixed 48 kHz, up to 8 push streams (4–192 kHz, mono/stereo, U8/S16, per-stream ring + gain), linear resampler, clipping, underrun = silence + counter, pause/clear/stats | first cut | `audio_test mix`: 11025 U8 mono + 22050 mono + 32000 + 44100 stereo at once, each measured within 0.01 % of nominal, 0 underruns while fed; `cycles 1000`: 0 failures; blocked `write()` released by STOP in < 0.4 s; heap at baseline after every run |
| **AUDI v1** interface + SDK wrappers (`papp_audio_*`) | done | as above |
| **2.5 Input manager** first cut (`lx_input`): USB HID host (several devices), gamepads in a standard layout for 4 players (PS3/PS4/PS5/generic/PSX-adaptor parsers from the reference; a report-descriptor check tells gamepads from other HID interfaces), boot keyboards with US text, one event queue (flushed per app run), HOME / Select+Start (1 s) → stop app | done | DualShock 3 detected as P1 (`054C:0268`); injected pad/key/text arrive as events in `input_test` ("Hi, P4!" typed correctly); HOME and a 1.5 s chord stop the app, a 0.5 s chord does not; heap steady over repeated runs. Real DS3 buttons show the right names on the status screen (checked by hand) |
| **INPT v1** interface + SDK wrappers (`papp_input_*`); DevLink `INPUT` (inject pad/key, read state) + `papp_dev.py input …` and `deploy --inject` | done | as above |
| DevLink `SHOT` (RLE16, host writes landscape PNG) | done | 800x480 screenshot in 0.4 s; buffer freed afterwards |
| Board buttons: BSP `bsp_buttons_init/read` + `bsp_analog_read` (standard-layout bits); Guition: custom GPIO pad add-on (detect via pull-up on GPIO 29, joystick + A/B on shared ADC2 pins, R1 (GPIO 29, also the detect pin)/L1 (30)/X/Y/Start/Select digital, stuck-pin check) and paddle (GPIO 51); lx_input polls at 5 ms with a 2-sample debounce and merges the pad into player 1; INPT v2 `analog()` (`papp_input_analog`), paddle in DevLink input state | done | pad detected (`caps` 127), no phantom presses at rest, paddle readable (±2/255 noise), USB pad and injection still P1. All buttons checked by hand (L1/R1 were swapped vs. the reference notes, fixed) |
| **THRD v1**: threads (create/join/exit/self, stack size + internal/PSRAM, priority class, core), mutex (+recursive), counting semaphore, queue, per-thread TLS slot, `app_exit` from any thread; all tracked: live threads stopped with the main task, objects freed at exit; blocking waits never hold up teardown. SDK `papp_thread_*`, `papp_mutex_*`, `papp_sem_*`, `papp_queue_*`; with `--libc`: per-thread `errno`/stdio state and real newlib locks (one recursive app mutex); `exit()` from a worker ends the app | done | `thread_test`: 4 threads on both cores count to 200000 under a mutex; 2000-item queue across cores in order; 1000 semaphore round trips 14 us each; per-thread errno + concurrent printf; join timeout + `thread_exit` code; 200 create/join cycles 112 us each with internal heap unchanged; 16-thread limit; `leave` (3 threads incl. blocked ones), `exit` (worker `exit(7)` → 7) and `spin` (spinners on both cores, stopped by HOME) all end with the heap at baseline; gfx/audio tests unchanged. Mutex lock+unlock costs ~4.9 us uncontended (FreeRTOS mutex via the interface) |
| **FAST region** (Spike B, ADR D3): `PAPP_FAST` / `PAPP_FAST_DATA` / `PAPP_FAST_BSS`; FAST linked at `0x10000000`; packer: region-tagged relocations + cross-region `PCREL_HI20/LO12` and `DIFF32` (jump tables); loader: FAST in executable internal SRAM (`MALLOC_CAP_EXEC`, PMP split off, cap `CONFIG_LX_APP_FAST_MAX_KB` = 192) or after SLOW in PSRAM when it doesn't fit; crash report maps FAST PCs back to the ELF | done | host test: hello / libc_test / fast_bench relocated == linked directly at 3 (SLOW, FAST) address pairs, byte for byte. Device (`fast_bench`, best of 3, PSRAM vs FAST, 128 KB L2): interpreter 200k ops 16.9 / 18.2 ms (x0.92, L1-resident); crc32 64 KB with table in FAST 2.69 / 2.25 ms (x1.19); 32 KB of code x200 19.5 / 17.8 ms (x1.09); emulator-like frame (interpreter + 32 KB code + PSRAM streaming): 32 KB/frame x0.98, **128 KB/frame 2.45 / 1.81 ms per frame (x1.35)**, 512 KB/frame x1.10. FAST pays off when other memory traffic evicts code from L2; with a quiet cache PSRAM code is as fast. Fallback (FAST in PSRAM) runs correctly. L2 256 KB: PSRAM-only 128 KB/frame case 1.74 ms/frame, but internal free 490 → 360 KB |
| Not yet | — | input: gamepad mapping DB + first-use mapping UI, mouse, keyboard layouts, rumble, pad hot-plug, battery level (GPIO 53); audio: pull model (THRD now exists), better resampler, launcher volume UI; compositor: async present (app draws while PPA runs), multiple surfaces, L8+palette, overlay layer; THRD v2: event flags, timers, fast user-space mutex (uncontended lock in ns, not us); L2 cache size decision with a real emulator (D4); `PAPP_MEM_EXEC` for dynarecs; CMake SDK; `INPUT` |

Known gaps: internal RAM is executable and writable (PMP split off for FAST, no W^X); PPA scale is quantised to 1/16, so STRETCH can leave a few pixels (160x100 → 800x475); a worker thread at PAPP_PRIO_HIGH spinning on core 1 can starve DevLink (prio 4) while it runs (HOME still stops it); `deploy` doesn't
yet notice a device reboot (it waits for the timeout); MEM `realloc` drops alignment.

---

## Overview

| Phase | Title | Est. | Depends on | Can overlap with |
|---|---|---|---|---|
| 0 | Foundations & technical spikes | 2 pw | — | — |
| 1 | BSP layer + bring-up on the reference board | 2–3 pw | 0 | 3 (design part) |
| 2 | Core launcher services | 4 pw | 1 | 4 |
| 3 | ABI v2, loader, App Manager | 3 pw | 0, 2 (partially) | 4 |
| 4 | SDK, toolchain & conformance suite | 3 pw | 3 (headers) | 2, 3, 5 |
| 5 | Launcher UI, overlay, dev tools | 3 pw | 2, 3 | 4, 6 |
| 6 | Port existing PAPPs to the SDK | 3–4 pw | 3, 4 | 5 |
| 7 | Emulators-as-PAPP pilot (decision gate) | 3–5 pw | 6 | 8 |
| 8 | Second board port (portability proof) | 2–3 pw | 6 | 7 |
| 9 | Hardening, ABI freeze, release 1.0 | 2–3 pw | all | — |
| **Total** | | **27–33 pw** | | ≈ **15–18 calendar weeks with 2 devs** |

Suggested split for two developers:
- **Dev A (platform)**: 0 → 1 → 2 → 5 → 8
- **Dev B (ABI/SDK/apps)**: 0 → 3 → 4 → 6 → 7
- Both: 9.

Milestones:

| Milestone | Content | End of |
|---|---|---|
| **M1 "Hello, relocated"** | A relocated `.papp` runs at an arbitrary address on the new launcher skeleton | Phase 0 + 1 |
| **M2 "Services online"** | Compositor, mixer, input, FS, arenas working; loader v2 + App Manager; SDK builds template | Phase 2 + 3 + 4 |
| **M3 "Feature parity"** | New launcher UI runs Doom/Quake/Duke3D/OpenTyrian/LVGL from the SDK, no leaks | Phase 5 + 6 |
| **M4 "Portable"** | Same `.papp` binaries run on a second board; emulator decision made | Phase 7 + 8 |
| **M5 "1.0"** | ABI v2 frozen, docs, release images | Phase 9 |

---

## Phase 0 — Foundations & technical spikes (2 pw)

Goal: remove the big unknowns before writing production code. Each spike ends with a short ADR in
`docs/adr/` (decision, measurements, consequences).

**0.1 Repository & conventions** (2 d)
- Git repo layout: `launcher/` (ESP-IDF project), `boards/`, `components/` (launcher core
  components), `papp-sdk/`, `apps/`, `tools/`, `docs/`, `SDcard/` (staging mirror, as in reference).
- `.clang-format`, naming conventions (`papp_*` = ABI, `lx_*` = launcher internals, `bsp_*` = board).
- Build scripts (PowerShell + Python), reuse `Resolve-IdfEnv.ps1` from the reference.
- CI (GitHub Actions, `espressif/idf:v5.5.2` container): build launcher for every board config, build
  SDK + all apps, run host unit tests of `papp-pack`.

**0.2 Spike A — relocatable images** (3 d)
- Link the reference template with `-Wl,-q` at base 0; write a prototype `papp-pack` that extracts
  `R_RISCV_32` relocs; prototype loader applies them at the address `esp_mmu_map()` returns.
- Test: same binary loaded twice at two different addresses in one session; a test app with function-
  pointer tables, string-pointer arrays and `.init_array`.
- Exit: works; list of relocation types that appear in real code (run the packer over the Quake and
  Doom ELFs from the reference to see the full set).

**0.3 Spike B — executable internal SRAM + performance** (3 d)
- Option (a): `CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n`, allocate an arena, execute from it.
  Option (b): keep split; reserve a region in the launcher linker script and open it with a PMP entry.
- Benchmark a CPU-bound kernel (e.g. a 6502/Z80 interpreter loop from nofrendo/smsplus, plus a
  memcpy/blit loop) from: flash (IDF build), IRAM, PSRAM-XIP, FAST arena; L2 cache 128 KB vs 256 KB.
- Exit: ADR D3/D4 with numbers; decision on default FAST arena size.

**0.4 Spike C — forced teardown** (2 d)
- App with 3 threads (one blocked on a queue, one spinning, one in `delay`) + allocations + open files;
  launcher kills and reclaims; repeat 500×; check heap returns to baseline.
- Exit: ADR D5; list of FreeRTOS gotchas (deleting tasks holding mutexes, static TCB/stack ownership).

**0.5 Spike D — libc in apps** (2 d)
- Link toolchain newlib + `-lstdc++` (minimal) into a test app; implement retarget locks via a mutex
  service; verify `printf`, `fopen`, `qsort`, `strtod`, `new/delete`, static constructors.
- Exit: ADR D6; known-good link line for the SDK.

---

## Phase 1 — BSP layer + bring-up on the reference board (2–3 pw)

**1.0 DevLink service** (3 d, done first — it accelerates everything after it) — framed protocol over
USB-Serial-JTAG, log routing, file commands (`LS/PUT/GET/RM/MKDIR`), `RUN/STOP/STATUS`, `INFO`,
`REBOOT`, crash record; host tool `tools/papp_dev.py` with `deploy`. `SHOT` and `INPUT` are added in
Phase 2 once the compositor and input manager exist. See ARCHITECTURE §7.8.

**1.1 Define `bsp_*.h`** (2 d) — display, backlight, touch, audio, buttons/analog, SD, USB host
power, battery, capability descriptor (see ARCHITECTURE §7.1). Review together with Phase 3 owner.

**1.2 Board package `boards/guition_p4_43`** (5 d) — port from the reference project:
- `st7701_lcd` + MIPI-DSI init → `bsp_display_*` with 2–3 DMA framebuffers in PSRAM and vsync callback.
- `gt911_touch` → raw points + transform descriptor.
- ES8311/I2S from `odroid_audio` → `bsp_audio_*` (fixed rate).
- GPIO pad incl. per-pin stuck detection (`odroid_input.c`), paddle ADC.
- SDMMC mount with FATFS LFN config.
- `sdkconfig.defaults` per board (PSRAM HEX 200 MHz, cache, FATFS LFN, USB host).

**1.3 Board package `boards/guition_p4_hdmi`** (3 d) — LT8912 bridge + shared I2C (reference
`HDMIport.md`), no touch/paddle caps. Removes every `CONFIG_HDMI_OUTPUT` `#ifdef` from core code.

**1.4 Built-in hardware self-test** (2 d) — launcher boot option that shows colour bars + touch
crosshair + button matrix + tone sweep + SD speed test. Reused for every future board port.

Exit criteria: self-test passes on LCD and HDMI; boot to self-test < 1.5 s.

---

## Phase 2 — Core launcher services (4 pw)

Each service is an ESP-IDF component in `components/lx_<name>/`, independent of the loader, with an
on-device test runner (Unity) so it can be validated before any app exists.

**2.1 Memory arenas** (3 d) — app PSRAM arena + FAST arena on `multi_heap`/TLSF; abstract caps
(`FAST/DMA/LARGE`), aligned allocations (64 B for DMA/PPA), stats, high-water marks, leak report.

**2.2 Threads & sync with tracking** (3 d) — wrappers for task (internal or PSRAM stack, like the
reference `svc_task_create`), mutex, recursive mutex, semaphore, queue, event flags, software timers;
priority classes; resource lists per app context.

**2.3 Compositor** (6 d)
- Surfaces (RGB565, RGB888, L8+palette), 1–3 buffers, scale modes (FIT_ASPECT/INTEGER/STRETCH/NONE),
  transform from BSP panel orientation; one PPA SRM per present; letterbox clear only on change.
- Overlay layer (ARGB) blended with PPA; vsync-paced flip; frame stats (FPS, present latency).
- L8→RGB565 converter (PIE SIMD path + C fallback).
- Touch inverse-transform into surface coordinates.
- Test: 320×200, 320×240, 400×240, 256×224, 640×480, 800×480 surfaces on LCD and HDMI.

**2.4 Audio engine** (5 d) — fixed-rate I2S writer task (core 1, high prio, internal DMA buffers),
N streams with ring buffers, linear (later polyphase) resampler, mixer, master volume/mute, push +
pull models, underrun counters. Test: 11 025/22 050/32 000/44 100 Hz streams simultaneously; 1 000
open/close cycles with no glitch or deadlock.

**2.5 Input manager** (5 d) — port `usb_host_hid` + `gamepad` (incl. auto-mapping DB), add HID
keyboard (boot + report protocol) and mouse; board buttons + paddle; logical gamepads (4 players,
standard layout + axes); event queue; system chord detection (HOME/MENU → launcher).

**2.6 Storage** (2 d) — tracked file/dir handles, sandbox path helpers (`app_dir`, `data_dir`),
`stat` with real sizes, rename/remove/mkdir.

**2.7 KV settings** (1 d) — per-app store (`/sd/data/<id>/settings.kv` or NVS namespace hash) + global prefs.

**2.8 Gfx / image services** (3 d) — generalise reference `sprite_blit`/`fb_copy`: PPA SRM/blend/fill
jobs (sync + async), 2D-DMA/async memcpy, PNG decode (pngdec), HW JPEG decode. No board-specific
colour swaps.

Exit criteria: all service unit tests green on both board configs; audio + compositor run 1 h soak.

---

## Phase 3 — ABI v2, loader, App Manager (3 pw)

**3.1 ABI headers** (3 d) — `papp_abi.h` (header, manifest TLVs, import entries, reloc encoding,
interface IDs/version rules) + one header per interface. Design review with both devs; mark as
`v2-preview`.

**3.2 Loader v2** (5 d) — header/manifest parse, compatibility check before allocation, SLOW + FAST
region allocation, streaming read, relocation (ABS32, cross-region PCREL/CALL pairs), import
resolution, cache maintenance + exec mapping, CRC verification. Load-time profiling (target: 1 MB
image < 300 ms from SD).

**3.3 App Manager** (4 d) — app context, `papp_main` task creation from manifest, lifecycle events
(PAUSE/RESUME/EXIT_REQUEST), normal and forced teardown, heap-baseline check, argv passing.

**3.4 Crash reporting** (2 d) — panic hook → `.noinit` crash record → post-reboot report screen +
`/sd/launcher/crash/*.txt`; optional task watchdog for `papp_main`.

Exit criteria: conformance stubs load at arbitrary addresses; **1 000× load → run → exit loop with
zero heap delta**; forced quit works for an app stuck in an infinite loop.

---

## Phase 4 — SDK, toolchain & conformance suite (3 pw)

**4.1 Runtime library** (5 d) — `crt0`, constructors/destructors, newlib syscalls (single shared
implementation replacing the four `papp_syscalls.c`), malloc routing policy, retarget locks, minimal
C++ runtime, generated interface stubs + import table, `PAPP_MANIFEST()` macro, `PAPP_FAST` attributes.

**4.2 Build system** (3 d) — CMake toolchain file + `papp_add_app()` function; also a PowerShell
wrapper for people who don't want CMake. Linker script with SLOW/FAST output sections.

**4.3 Tools** (4 d) — `papp-pack` (validation: unsupported relocs, forbidden sections, missing
manifest, size report per region), `papp-upload` / `papp-run` / `papp-log` (extends reference `PAPU`
serial protocol), `papp-symbolize` (crash address → function:line).

**4.4 Templates & docs** (2 d) — hello, sprite demo (port of `ESP32_P4_PAPP_Template`), C++ app, LVGL
app; `docs/SDK_GUIDE.md` replacing `PAPP_GUIDE.md`.

**4.5 ABI conformance suite** (3 d) — one PAPP that exercises every function of every interface
(including error paths and optional-interface absence) and prints a PASS/FAIL table over serial and on
screen. **This is the acceptance test for every launcher release and every board port.**

Exit criteria: one-command build of all templates on Windows and in CI; conformance suite 100% on
both board configs.

---

## Phase 5 — Launcher UI, overlay, dev tools (3 pw)

**5.1 LVGL integration** (2 d) — LVGL v9 on the compositor (launcher draws into its own surface; UI
torn down/parked while an app runs).

**5.2 Home screen** (4 d) — app grid/carousel, icons, details page, index cache with fast rescan,
greyed-out incompatible apps with the reason ("needs touch", "needs AUDIO v3").

**5.3 File browser + associations** (3 d) — browse `/sd/roms/...`, filters from manifests, favourites
and recents (carry over the reference concepts), launch with argv.

**5.4 Overlay menu** (3 d) — HOME chord → pause app → menu (resume, volume, brightness, app items,
screenshot, quit); volume/brightness OSD; UI dialogs for the `UI` interface (file picker, message box,
on-screen keyboard).

**5.5 Settings & gamepad mapping UI** (2 d).

**5.6 Dev mode** (1 d) — serial upload/run/log/crash fetch while in the launcher.

Exit criteria: full navigation with gamepad, touch and keyboard; boot-to-home < 2 s with 50 apps.

---

## Phase 6 — Port existing PAPPs to the SDK (3–4 pw)

Order chosen from simplest to hardest; each port deletes its private libc glue and watchdog/exit code.

| App | Work | Est. |
|---|---|---|
| Template + touch test | rewrite on SDK | 1 d |
| LVGL demo | display/indev/tick on SDK, drop `papp_libc.c` | 2 d |
| **sdl2-lite** library | SDL video/audio/events/timer subset on ABI (from OpenTyrian/Duke3D shims) | 4 d |
| OpenTyrian | on sdl2-lite | 2 d |
| Duke3D | on sdl2-lite | 3 d |
| Doom (prboom-go) | `rg_*` stubs → ABI; exit via lifecycle event instead of longjmp watchdog | 3 d |
| Quake | same; 256 KB stack via manifest; hunk in LARGE arena | 3 d |

Perf check: FPS and load time vs the reference launcher for each game; profile hot functions and
tag them `PAPP_FAST` where it pays off.

Exit criteria: all apps run, quit via overlay, **100 consecutive launches without reboot or heap
growth**, FPS ≥ reference.

---

## Phase 7 — Emulators-as-PAPP pilot (3–5 pw, decision gate)

Goal: find out whether the OTA/reboot model can be retired.

1. **NES (nofrendo)** first — small, well understood (1 pw). Use `argv[1]` ROM path, push audio
   stream, L8 surface, `PAPP_FAST` on the CPU/PPU core.
2. **SNES (snes9x) or Genesis (gwenesis)** — the most demanding 60 FPS cores (2–3 pw). Port the
   reference frame pacer and per-emulator audio strategy; profile; move hot code into FAST.
3. **Gate review**: if ≥ parity (60 FPS, no audio crackle) → plan migration of the remaining cores
   (separate plan, ~1–2 pw each). If not → keep an "OTA native firmware" app type in the launcher
   (manifest type `firmware`, launcher flashes/boots it) for the heavy cores and revisit later.

Save states map to the overlay's app menu items and `/sd/data/<id>/saves/`.

---

## Phase 8 — Second board port (2–3 pw)

Pick a board that is **meaningfully different** from the reference to stress the abstraction — e.g.
a 7″ 1024×600 landscape P4 board (Espressif ESP32-P4-Function-EV-Board or a Waveshare ESP32-P4
board), different codec, no GPIO pad (USB-only input).

1. New `boards/<id>/` package (wrap the vendor BSP where one exists) — 1–1.5 pw.
2. Run self-test + conformance suite — fix *launcher core* bugs found (expected!).
3. Copy the SD card from the reference board **unchanged**: every `.papp` must run byte-identical.
4. Write `docs/PORTING_GUIDE.md` from the experience (checklist, BSP contract, test procedure).

Exit criteria: conformance 100%; all Phase 6 apps run unmodified; porting guide reviewed.

---

## Phase 9 — Hardening, ABI freeze, release 1.0 (2–3 pw)

- Soak tests: 24 h app cycling; SD removal while running; low-memory apps; power-loss during save.
- Fault injection: apps that crash, hang, leak, spam logs, open 1 000 files → launcher always recovers.
- Load-time and boot-time optimisation.
- **Freeze ABI v2** (remove `preview`), publish versioning policy (interface minor/major rules).
- Launcher self-update from SD (A/B OTA).
- Release packaging: merged firmware per board, SD-card image, SDK zip, docs (User guide, SDK guide,
  Porting guide, Architecture).

---

## Backlog (post-1.0)

- `NET` interface: Wi-Fi via esp_hosted (P4 boards with a C6 companion); app download / "store";
  Wi-Fi upload in dev mode.
- `BTHD`: BLE HID gamepads/keyboards.
- `USBD`: USB MSC mode to expose the SD card to a PC.
- **PC simulator**: Windows/Linux host that implements the ABI on SDL2 and loads apps built as
  native DLLs — fast iteration without hardware.
- `MEM_EXEC` for dynarec/JIT emulators.
- `CAM`: MIPI-CSI camera + HW JPEG/H.264 encode.
- Multi-language launcher UI, themes.

---

## Immediate next steps (week 1)

1. Agree on this architecture (especially D1, D2, D6 in ARCHITECTURE §11) and the board ID naming.
2. Create the repository skeleton + CI (0.1).
3. Start Spikes A and B in parallel — they decide the binary format and whether emulators can follow.
4. Order the second board for Phase 8 now, so it's on the desk when needed.
