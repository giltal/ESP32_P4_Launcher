# Hands-on test checklist — emulators and games (launcher 0.11.0)

Fill in each cell with ✅ / ❌ (+ a short note). Start every app from the home screen.
HOME on the pad (or Select+Start held 1 s) force-stops any app; use it only if the app's own exit fails.
Emulators: Exit game → the ROM browser again (on the same ROM); B in the browser → home screen.
GB/GBC, PCE and Spectrum also save the state on Exit game (as in RetroESP32), overwriting that ROM's save.

## How each app is driven

| App | ROM folder | Menu opens with | Volume with | In-game menu items | Exit game goes to |
|---|---|---|---|---|---|
| NES | /sd/roms/nes | X | (none yet) | Resume, Save, Load, Restart, Delete save, Exit | ROM browser |
| Game Boy (gb) | /sd/roms/gb | X | Y | Resume, Save, Overwrite, Reload, Restart, Delete save, Exit | ROM browser |
| Game Boy Color (gbc) | /sd/roms/gbc | X | Y | as GB | ROM browser |
| Master System (sms) | /sd/roms/sms | X | Y | as GB | ROM browser |
| Game Gear (gg) | /sd/roms/gg | X | Y | as GB | ROM browser |
| ColecoVision (col) | /sd/roms/col | X | Y | as GB | ROM browser |
| PC Engine (pce) | /sd/roms/pce | X | Y | as GB | ROM browser |
| Atari 7800 (a78) | /sd/roms/a78 | X | Y | as GB | ROM browser |
| ZX Spectrum | /sd/roms/spectrum | X | Y | as GB | ROM browser |
| Atari 800 (a800) | /sd/roms/a800 | X | Y | as GB | ROM browser |
| Atari 2600 (a26) | /sd/roms/a26 | X | Y | Resume, Save state, Load state, Exit | ROM browser |
| Atari Lynx | /sd/roms/lynx | X | Y | Resume, Save state, Load state, Exit | ROM browser |
| Genesis | /sd/roms/gen | touch left edge | touch right edge | Resume, Save state, Load state, Exit | ROM browser |
| SNES | /sd/roms/snes | touch left edge | touch right edge | Resume, Save state, Load state, Exit | ROM browser |
| Neo Geo | /sd/roms/neogeo | touch left edge | touch right edge | Resume, Save state, Load state, Exit | ROM browser |
| Doom | /sd/roms/doom (WAD) | X | — | Doom's own menu (new game, load / save game); hold X 3 s = exit | Home screen |
| OpenTyrian | /sd/tyrian/data | X | Y | OpenTyrian's own menu; hold X 3 s = exit | Home screen |
| Quake | /sd/roms/quake/id1 | touch left edge | touch right edge | Quake's own menu (Esc); hold the left edge 3 s = exit | Home screen |
| Duke Nukem 3D | /sd/roms/duke3d | touch left edge | touch right edge | Duke's own menu; hold the left edge 3 s = exit | Home screen |

On SNES, Genesis and Neo Geo, X and Y are game buttons (Neo Geo: A B C D = A B X Y, Select = coin).

## Results

Cells filled by Claude (2026-10-30) were driven over DevLink: injected buttons + screenshots, saves
checked on the SD card (and restored afterwards). Sound = audio stream running + underruns from the
launcher's counters, not heard. Controls = only that START reaches the game. "by hand" = for you. Open issue: one board reset during testing, `abort()` in the
USB HID host task while GBC ran (cause not known yet).

| App | Starts (browser → game) | Picture (size, colours, no tearing) | Sound | Controls (D-pad, all buttons, P2 if any) | Menu opens / closes | Save | Load / Resume from browser | Volume overlay | Exit from menu | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| NES | | | | | | | | — | | |
| GB | ✅ Alleyway | ✅ 59.7 fps | stream ✅ 4 underruns/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload; browser Resume ✅ | ✅ | ✅ → browser → B → home | |
| GBC | ✅ SMB Deluxe | ✅ 59.4 fps | stream ✅ 4/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | |
| SMS | ✅ Sonic Chaos | ✅ 60 fps | stream ✅ 1/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | |
| Game Gear | ✅ Choplifter III | ✅ 60 fps (24-line offset fixed) | stream ✅ 1/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | |
| ColecoVision | ✅ Defender | ✅ 60 fps | stream ✅ 2/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | BIOS.col hidden in the browser |
| PC Engine | ✅ Vigilantes | ✅ 60 fps | stream ✅ 0/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | 1941 is SuperGrafx: not supported |
| Atari 7800 | ✅ Alien Brigade | ✅ draws every 2nd frame (30 fps, as the reference) | stream ✅ 3/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | |
| ZX Spectrum | ✅ Monty | ✅ 50 fps | stream ✅ 0/20 s | rest by hand (keys via Y keyboard) | ✅ | ✅ | ✅ Reload | — Y = on-screen keyboard (reference) | ✅ | |
| Atari 800 | ✅ Bruce Lee (ATR) | ✅ 60 fps | stream ✅ 2/20 s | START ✅ (injected); rest by hand | ✅ | ✅ | ✅ Reload | ✅ | ✅ | Air Battle.atr: blue screen (needs BASIC?) |
| Atari 2600 | ✅ Combat | ✅ 60 fps | stream ✅ 2/20 s | START ✅ (injected); rest by hand | ✅ (drawn on black) | ✅ | ✅ Load state | ✅ | ✅ | too slow: to investigate |
| Lynx | ✅ Battle Wheels | ✅ 59.3 fps | stream ✅ 0/20 s | START ✅ (injected); rest by hand | ✅ (drawn on black) | ✅ | ✅ Load state | Y steps volume (brief indicator) | ✅ | |
| Genesis | ✅ Aladdin | ✅ emulates 60 fps, draws ~54 (frame skip) | stream ✅ 6/20 s | by hand | touch: by hand | by hand | by hand | touch: by hand | by hand | |
| SNES | ✅ ActRaiser | ✅ emulates 60 fps, draws ~35 (frame skip) | stream ✅ 8/20 s | by hand | touch: by hand | by hand | by hand | touch: by hand | by hand | |
| Neo Geo | ✅ Metal Slug (15 s load) | ✅ 60 fps with frame skip | stream ✅ | by hand | touch: by hand | by hand | by hand | touch: by hand | by hand | first run of a big game builds its sprite cache (slow) |
| Doom | ✅ demo | ✅ 33 fps | stream ✅ 0/25 s | START ✅; Y = automap | ✅ X opens / closes | by hand | — | — | ✅ hold X 3 s | |
| OpenTyrian | ✅ title | ✅ | stream ✅ 1/25 s | by hand | X on the title menu = quit (Esc) | by hand | — | ✅ | ✅ hold X 3 s (and X on title) | |
| Quake | ✅ demo | ✅ 20.2 fps timedemo (original ~21) | stream ✅ 1/25 s | START opens menu ✅ | touch: by hand | by hand | — | touch: by hand | touch: by hand (HOME works) | |
| Duke Nukem 3D | ✅ title | ✅ 41 fps in E1L1 (original ~39) | stream ✅ 1/25 s | by hand | touch: by hand | by hand | — | touch: by hand | touch: by hand (HOME works) | |

Also once, from the home screen: the launcher volume setting carries into the apps, and every app
returns to the home screen with that app first; heap back to baseline (`papp_dev.py status`).
