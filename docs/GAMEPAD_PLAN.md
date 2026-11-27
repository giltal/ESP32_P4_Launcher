# Better controller support: plan (2026-11-26)

Goal: flight sticks, throttles and wheels with all their axes and buttons; the Wii Remote (and its Nunchuk and
Classic Controller) over Bluetooth; and a picture of the controller in the launcher, so mapping a pad means
pressing what is highlighted. Before the bigger-app-slots migration (`docs/ROADMAP.md`).

## Where we are (0.37.1)

| Part | Today | Limit |
|---|---|---|
| USB HID pads (`lx_hid_parse.c`) | Descriptor parser for joystick (0x04) and gamepad (0x05) collections | 6 axes (X Y Z Rx Ry Rz), 16 buttons, 1 hat, the first report id only. No Slider, Dial or Wheel; no Simulation page (throttle, rudder, steering, accelerator, brake). A flight stick's throttle and most of its buttons are lost |
| Known pads | Sony, Switch Pro, XInput read by model | Xbox One (GIP) not supported |
| What apps see (INPT v3) | `papp_gamepad_t`: 2 sticks, 2 triggers, 18 standard buttons | No place for twist, throttle, rudder, a second hat or button 19+; no motion data from pads; no vibration (nothing is ever sent to a pad) |
| Mapping (`gamepads.txt`) | Button remaps per VID:PID | No axis mapping, no device kind, no picture |
| Bluetooth pads (`lx_bt_pads.c`) | Classic HID host (USB dongle) + BLE HID (dongle or C6) | Wii Remote: needs its own setup (report mode, LEDs, PIN pairing), not done |
| Map controller screen | Asks for each button in turn, by name | No picture |

## Phases

Each phase ends with a board test, a commit and a short note here. Order = what is testable soonest.

### 1. Joysticks, throttles and wheels in the parser (M)

- `lx_hid_parse`: up to 8 axes with their role: X, Y, Z, Rx, Ry, Rz, Slider, Dial, Wheel, and from the Simulation
  page (0x02) Throttle, Rudder, Steering, Accelerator, Brake. Up to 32 buttons, up to 2 hats, several report ids
  (a device that splits its state over reports), multi-axis controller collections (0x08).
- Tests in `apps/hid_test` (host + board): descriptors written to match the public layouts of common devices
  (a 4-axis flight stick with a slider throttle, a HOTAS with Z throttle and rocker, a wheel with pedals, a
  gamepad that uses ids). The existing gamepad cases must keep passing.
- The standard pad gets sensible defaults, so apps that only know `papp_gamepad_t` still work with a stick:
  X/Y → left stick, twist (Rz) → right stick X, throttle → right stick Y (or RT), trigger → A, thumb button → B,
  hat → d-pad.

### 2. INPT v4: every axis and button for apps (M)

- Appended to INPT (v4), nothing existing changes:
  `bool joystick(int player, papp_joystick_t *out)`: device kind (gamepad, flight stick, throttle, wheel,
  Wii Remote...), up to 8 axes each with its role (`PAPP_AXIS_THROTTLE` ...), 32 buttons, 2 hats.
  `bool motion(int player, papp_motion_t *out)`: accelerometer (and later gyro / IR pointer) of pads that have one.
- `gamepads.txt` grows optional keys on a pad's line: `kind=flightstick`, axis remaps (`Rz=RX`, `Slider=-RY` with
  `-` = inverted), `pic=<picture name>`. Old files keep working.
- DevLink `input state` prints every axis by role; `input_test` shows all of them.
- A **Controller test** app (Store, Tools): one screen per player with live axes, hats and buttons, so a user can
  see what their device reports and check a mapping.

### 2b. Vibration (rumble) and lights (M)

Pads only receive today; this adds the way back (output reports) on every transport.

- INPT v4 `rumble(int player, uint16_t strong, uint16_t weak, uint32_t ms)`: two motors (heavy and light), 0..65535,
  stops by itself after `ms` (0 = until changed), and when the app ends. A pad without motors ignores it, and
  `papp_joystick_t` says whether it has them.
- Per model:
  - DualShock 4: USB report 0x05; Bluetooth report 0x11 (needs a CRC-32 at the end).
  - DualSense: USB report 0x02; Bluetooth 0x31 (CRC-32).
  - XInput (Xbox 360): the 8-byte rumble command.
  - Switch Pro: its HD rumble, a simple fixed pattern first.
  - Wii Remote: the rumble bit in its output reports (phase 3).
  - Generic HID pads with a rumble output report (PID force feedback on flight sticks and wheels: later, only if a
    device at hand has it).
- Transports: USB interrupt OUT (or SET_REPORT), Bluetooth Classic HID data, BLE HID output report.
- Same reports carry the DS4 / DualSense light bar and the player LEDs: set them to the player number's colour when
  a pad connects (and `led()` for apps, if it costs little).
- The launcher uses it too: a short buzz when a pad connects and gets its player number.
- First target tonight: the DualShock 4 that is connected to the 7" v3 over Bluetooth, then over USB.

### 3. Wii Remote over Bluetooth (L)

Needs a Bluetooth Classic USB dongle (the C6 is LE only). Protocol facts to build on: the remote is a Classic HID
device ("Nintendo RVL-CNT-01", newer ones "-TR" with MotionPlus inside); output reports go on the interrupt channel;
report 0x12 picks the data mode, 0x11 the LEDs, 0x15 asks for status (battery, extension); extensions are set up
by writing 0x55 to 0xA400F0 and 0x00 to 0xA400FB, then reading the id at 0xA400FA.

- Found by name in the Bluetooth scan. Pairing: 1+2 connects for the session; the red SYNC button pairs for good
  (legacy PIN = the board's address reversed), after which any button reconnects it.
- After connecting: data mode with buttons + accelerometer + extension, LED = player number, short rumble, battery
  from the status report.
- Default layouts: Remote alone held sideways (NES style: d-pad turned, 1/2 = B/A, A/B(trigger) = X/Y, +/- =
  Start/Select, HOME = HOME); with a Nunchuk, upright (stick = left stick, C/Z = shoulder buttons); Classic
  Controller = a normal pad. Accelerometer through `motion()`.
- Then, if the device is at hand: Wii U Pro Controller (same family, its own report).
- Risks: some -TR remotes and some dongles are picky about who starts the connection and about SDP. Test with the
  actual remote early, before the details.

### 4. Controller pictures (M)

- **Our own line drawings, by layout family**, not product photos or logos (those are someone else's copyright and
  trademarks): "PlayStation-style", "Xbox-style", "Switch-style", "SNES-style", "NES-style", "flight stick",
  "throttle", "wheel", "Wii Remote" (sideways and upright), "Wii Remote + Nunchuk", "arcade stick". Drawn by a
  script (`tools/make_pad_pictures.py`, Pillow) in the launcher's colours, so they're consistent and easy to add.
- Each picture has a small text file of button positions (`A=412,188` ...): the launcher highlights the button.
- On the card in `/sd/config/pads/` (in `SDcard/config/pads`), downloaded and kept current by the Store like
  `gamepads.txt`. `gamepads.txt` names a pad's picture (`pic=`); otherwise the kind chooses one.
- Used by: Map controller (the button to press is lit), Settings (the connected pads, by player), Bluetooth (a
  paired pad), Controller test.

### 5. Release (S)

Docs (ARCHITECTURE §7.5 input, CLAUDE.md, `papp_abi.h` comments, README of the SDK), `gamepads.txt` lines for the
devices tested, launcher 0.38.0 on all boards, Store data files (pictures), roadmap updated.

## Tonight

Boards: 7" v1 (COM10), 7" v3 (COM6, a DualShock 4 is connected over Bluetooth), Tab5 (COM15), all on 0.37.1.

1. Phase 1 complete, with tests, on the three boards (a USB pad that is plugged in keeps working: no regression).
2. Phase 2: the interface and the default mappings, `input state`, the Controller test app.
3. Phase 2b: rumble and the light bar on the DualShock 4 (Bluetooth, then USB), then XInput / DualSense if at hand.
4. Phase 3 as far as the hardware allows: pairing and buttons first, then extensions and motion.
5. Phase 4: the drawing script and the first pictures (the families of the devices at hand), shown in Map controller.

Phases 1-2 can be built and checked with test descriptors and DevLink alone; phase 3 and the real-device checks of
phase 1 need the devices plugged in or paired.

## Status (2026-11-27, overnight, uncommitted on claude/gamepads-v2)

Done and checked without the devices:
- Phase 1: parser by axis role (Desktop + Simulation pages, 2 hats, 32 buttons, 4 report ids), kind detection,
  flight-stick / wheel views, throttle joins the flight stick's player. `hid_test` (test descriptors, Wii scripts):
  0 failures on COM10.
- Phase 2: INPT v4 (`papp_input_joystick` / `_motion` / `_rumble`; the SDK falls back to `gamepad` on older
  launchers), DevLink `input state --all`, `input rumble`. Controller test app (Store, Tools; icon).
- Phase 2b: rumble + lights for DualShock 4 / DualSense (USB and Bluetooth reports with CRC), Switch Pro HD rumble,
  XInput, Wii Remote; 150 ms buzz on connect, player colours; stopped at app exit. On COM6 the DS4 over USB:
  motion reads 1 g, `input rumble` accepted, no output errors. **Not felt or seen yet.**
- Phase 3: Wii Remote, Nunchuk, Classic Controller, Wii U Pro: PIN pairing (SYNC and 1+2), extension set-up,
  sideways / upright layouts, rumble bit, accelerometer. Scripted only.
- Phase 4: `tools/make_pad_pictures.py` draws 9 families into `SDcard/config/pads` (.png + positions); Map
  controller shows the pad's picture and rings the asked button (checked on COM6 with the DS4: PlayStation
  picture). The Store keeps `/sd/config/pads` current ("pads" in apps.json; checked with a local catalogue).

Also done: launcher 0.38.0 moves boards to 6.5 MB app slots (ARCHITECTURE, "Flash layout" under self-update), tested on all three.

To test with the devices: real flight stick, throttle (and both together), wheel (axes, pedals, kind detection);
rumble and light bar felt/seen on DS4 (USB, Bluetooth), DualSense, Switch Pro, Xbox pad; Wii Remote pairing both
ways, Nunchuk, Classic Controller, motion. Then `gamepads.txt` lines (`kind=`, `pic=`, axis remaps) for them.

Not done: Bluetooth LE pads' output reports (Xbox BLE rumble), DS3 rumble; pictures in Settings / Bluetooth
screens; a picture in Controller test (apps have no PNG decoder).

## Open questions

- Which devices are at hand to test with (flight stick / HOTAS model, wheel, Wii Remote original or -TR, Nunchuk,
  Classic Controller, Wii U Pro)? And which board has the Bluetooth Classic dongle?
- Pictures as drawings by layout family (as above), rather than photos: OK?
- Should a flight stick + separate throttle (two USB devices) appear to apps as one player? (Proposal: yes, when
  both are plugged in, the throttle joins the stick's player.)
