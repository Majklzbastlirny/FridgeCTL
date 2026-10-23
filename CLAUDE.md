# CLAUDE.md — FridgeCTL

Project context for Claude Code. Read this first.

## What this is

Smart fridge controller firmware for a **LilyGo T-Relay (ESP32)**. It was
**migrated from ESPHome to native ESP-IDF + PlatformIO**. The original ESPHome
config is still in the repo at `fridgectl.yaml` — treat it as the **behavioural
spec / source of truth** for control logic. The native firmware in `src/`
reproduces it 1:1.

## Why the migration happened (the core problem)

The ESPHome build ran everything in one cooperative loop. The four bit-banged
DS18B20 OneWire buses, the 200 ms blocking CT-clamp RMS sample, and WiFi/API
reconnects all shared that single loop. A flaky/dead DS18B20 stalled the loop,
it overran, and the **Task Watchdog rebooted the board**. Symptom the user
reported: "99% are timeouts, then watchdog kills everything."

**The fix is the architecture**: blocking I/O is isolated into its own FreeRTOS
tasks. Only the 5 s `control` task is subscribed to the Task WDT, and it never
performs blocking I/O — it reads a shared state snapshot under a mutex and
drives relays. A stuck sensor now only delays the sensor task; the safety loop
keeps feeding the watchdog. **Do not reintroduce blocking calls into the
control path.**

## Build / flash / OTA

```
pio run                 # build (first build downloads toolchain, ~slow; after that ~2 min)
pio run -t upload       # flash over USB
pio device monitor      # serial @ 115200
```
- Build is **verified passing**: Flash 57.9% (~890 KB of 1.5 MB app slot),
  RAM 20.3% (66.6 KB — the jump from 13.2% is the 19 KB log ring in
  `logbuf.cpp`). Two OTA app slots (`partitions.csv`), 4 MB flash. Device runs at
  `192.168.1.75` (DHCP; it was `.4` until 2026-10, and HA's IP Address sensor
  always has the current one) and has been flashed + OTA-tested on real hardware.
- **OTA after first USB flash**: use the web UI (below) — browse to
  `http://<device-ip>/control` (Basic auth) → Firmware section → upload
  `.pio/build/fridgectl/firmware.bin`. Or POST it:
  `curl.exe -u admin:PASS --data-binary "@.pio/build/fridgectl/firmware.bin" http://<ip>/update`
  NOTE: in PowerShell `curl` is an alias for `Invoke-WebRequest`; use **`curl.exe`**
  and quote the `@file`. Relays are forced off before flashing; board reboots.
- **⚠️ Rollback can silently disappear on an IDF upgrade — check it after one.**
  On 2026-08-06 the platform auto-resolved to Espressif 32 55.3.39 / IDF 5.5.4
  and the regenerated `sdkconfig.fridgectl` silently dropped
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` — every *other* line of
  `sdkconfig.defaults` still applied, only this one was lost. Symptom: the boot
  log reads `web: image already valid, no rollback check needed` **after an
  OTA** (it is correct and expected after a USB flash). Re-enabled directly in
  `sdkconfig.fridgectl`; since rollback lives in the **bootloader** and OTA only
  replaces the app partition, it needed a one-time USB flash to take effect.
  ✅ **Re-armed and verified end-to-end 2026-08-06** — USB-flashed the new
  bootloader, then OTA'd and observed the full chain in the log:
  `image PENDING_VERIFY - running health check before commit` (1.1 s) →
  `health check PASSED - control loop alive, image committed` (38.2 s, i.e.
  `boot_done` at ~32 s plus two 5 s control beats). Quick check after any
  toolchain bump: `grep BOOTLOADER_APP_ROLLBACK_ENABLE sdkconfig.fridgectl`.
- **OTA rollback (safety net).** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. An
  OTA'd image boots in `PENDING_VERIFY`; `ota_health_check_start()` (task in
  ota.cpp) commits it with `esp_ota_mark_app_valid_cancel_rollback()` once the
  **control-loop heartbeat** `g.control_beats` advances ≥2 ticks with
  `boot_done` (i.e. the safety loop is genuinely running). ~90 s budget (covers
  the 30 s warmup); if it never goes healthy it calls
  `esp_ota_mark_app_invalid_rollback_and_reboot()` and the bootloader reverts.
  **WiFi/MQTT are deliberately NOT health criteria** — the fridge is designed to
  run fully offline, so a router/broker outage must never roll back a good image.
  (Earlier versions wrongly required wifi_connected — fixed.) `control_beats`
  also increments in the system-disabled branch so OTA-while-off still passes.
  USB flash is already valid → check is a no-op. Rollback costs ~0 flash
  (reuses existing `otadata`).
  **Two OTA slots are NOT the same as rollback** — the slots are storage, the
  rollback is policy, and they are independently configured:
  - The bootloader *always* falls back to the other slot if the selected image
    fails integrity verification (`try_load_partition()` in
    `bootloader_utility.c`). A truncated/corrupted upload is covered regardless
    of Kconfig.
  - **Only** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` covers an image that
    verifies fine but is broken at runtime (panic loop, hang, dead control
    loop). Without it `esp_ota_set_boot_partition()` writes ota_state
    `ESP_OTA_IMG_UNDEFINED` (see `set_new_state_otadata()`, esp_ota_ops.c) and
    the bootloader boots that slot forever.
  In that second case a boot-looping app never reaches the fallback AP or the
  web UI, so **recovery is USB-only**. Don't repeat "two slots + fallback AP =
  can't brick" — it was overstated.

## Web UI (two-stage, served by ota.cpp)

The HTTP server exposes a 2-stage interface (entity data comes from the same
`mqtt_net.cpp` tables as HA, via `web_build_state_json()` / `web_set_*()`):
- `GET /`           — **read-only dashboard**, no password. Polls `/api/state`
  every 3 s; shows all sensors, all binary status flags, and text/system info.
- `GET /api/state`  — full state as JSON, no password (read-only data).
- `GET /control`    — **full control surface + OTA**, Basic-Auth. All switches,
  all numbers (setpoints/config), action buttons, 1-Wire sensor browser, live
  log tail, firmware upload.
- `POST /api/cmd`   — control commands (Basic-Auth); JSON `{type,obj,val}` where
  type ∈ switch|number|button|rom. Tiny purpose-built JSON parser (`json_field`).
- `GET /api/onewire`— 1-Wire bus inventory as JSON (Basic-Auth).
- `GET /api/logs?since=N` — raw log tail, `text/plain`, **chunked** (Basic-Auth).
- `POST /update`    — firmware image (Basic-Auth).
Same `OTA_USERNAME`/`OTA_PASSWORD` from secrets.h gate all protected routes.
`max_uri_handlers` is 10 (7 in use). The control page's three generic sections
are re-rendered wholesale by `load()` every 5 s, so the 1-Wire and log panels
live in **their own containers with their own polls** — otherwise the re-render
would eat a click target mid-interaction.

## Environment gotchas (important for tooling)

- **Build with the project's own PlatformIO, not the global `pio` (2026-10-23).**
  ESPHome 2026.10.0 lives in the same user Python and pins
  `platformio==6.2.0`. Its SCons 4.11 breaks the pinned pioarduino 55.03.39
  platform at link time: `ModuleNotFoundError: No module named
  'SCons.Tool.FortranCommon'`, after everything has compiled. FridgeCTL
  therefore has an isolated PlatformIO 6.1.19 venv and core dir (machine-
  specific, so kept out of platformio.ini):
  ```
  $env:PLATFORMIO_CORE_DIR = "C:\pb\pio-core"; $env:PLATFORMIO_BUILD_DIR = "C:\pb\fc2"; C:\pb\pio619\Scripts\pio.exe run
  ```
  The image lands in `C:\pb\fc2\fridgectl\firmware.bin`. Recreate the venv
  with `python -m venv C:\pb\pio619` then
  `C:\pb\pio619\Scripts\python -m pip install platformio==6.1.19`.
- Windows 11, PowerShell default shell. Besides the venv above there are
  global PlatformIO cores (the user-site one is now 6.2.0 via ESPHome).
- **Run `pio` from PowerShell, never from the Bash/MSys tool.** ESP-IDF's
  `idf_tools.py` hard-refuses there ("MSys/Mingw is not supported") and the
  build dies before compiling anything.
- **`build_dir` must be short (2026-08).** With the current platform
  (Espressif 32 55.3.39 / IDF 5.5.4) the default
  `C:\Users\Michal\Documents\FridgeCTL\.pio\build\fridgectl\…` overflows the
  Windows 8191-char command-line limit while generating `sections.ld`:
  *"The command line is too long."* — after every source has already compiled
  fine, so it looks like a code failure but isn't. Build with
  `$env:PLATFORMIO_BUILD_DIR = "C:\pb\fc"; pio run`; the image lands in
  `C:\pb\fc\fridgectl\firmware.bin`. (Kept out of `platformio.ini` on purpose —
  it's a machine-specific absolute path and this repo is public.)
- `pio pkg list` / `pio platform list` **crash with UnicodeEncodeError** (cp1250
  console). Avoid them; use `pio run` directly.
- **Run builds in background** (`run_in_background: true`) writing to a log
  file, then wait with `until grep -q "EXIT_CODE=" log; do sleep 5; done`.
  Do NOT chain `sleep` before a `tail` (the harness blocks it). The first
  toolchain install takes many minutes — that already happened once, so it's
  cached now.

## Task model (FreeRTOS)

| Task         | Prio | Stack | Job |
|--------------|------|-------|-----|
| `control`    | 10   | 6144  | 5 s safety/control state machine. **Only Task-WDT subscriber.** Never blocks on I/O. |
| `input`      | 6    | 4096  | Door + button debounce, short/double/long-press classify |
| `leds`       | 5    | 4096  | 500 ms panel LEDs, status LED, interior light auto-off |
| `temp`       | 4    | 4096  | DS18B20 reads; `vTaskDelay` over the 750 ms conversion |
| `ct`         | 4    | 4096  | CT-clamp RMS; back-to-back sampling during 3 s inrush window |
| `telemetry`  | 3    | 6144  | Publishes all MQTT state every 5 s; refreshes WiFi RSSI |
| `nvs_commit` | 2    | 4096  | Flushes dirty persisted config every 5 min |

State sharing: everything lives in `AppState g` (app_state.h), guarded by one
**recursive** mutex. Use `StateGuard lock;` (RAII) for a coherent
read-modify-write. `control_tick()` runs entirely under the lock and does no
blocking I/O. `millis()` is an `esp_timer`-based ms counter (wraps ~49 days,
matching Arduino semantics the ESPHome logic assumed). **Deadlines (`*_until`)
must use `deadline_pending()` / `deadline_after()`** from app_state.h, never a
raw `(int32_t)(until - now) > 0`: with a 0 "off" sentinel that reads as ON
after 24.8 days of uptime. That is exactly how turbo latched on and froze the
chamber to −7 °C on 2026-10-12, a bug inherited from the ESPHome YAML.
Elapsed-time checks `(uint32_t)(now - start) > X` are wrap-safe as-is.

## Home Assistant integration (MQTT Discovery)

- On MQTT connect, firmware publishes **retained HA MQTT Discovery** configs for
  every entity, so they auto-appear under one `FridgeCTL` device. Availability
  via LWT on `fridgectl/status` (online/offline).
- Topics: state `fridgectl/<object>/state`, command `fridgectl/<object>/cmd`.
- Entities are **data-driven tables** in `mqtt_net.cpp` (`SENSORS[]`, `BINS[]`,
  `SWITCHES[]`, `NUMBERS[]`, `BUTTONS[]`, `TEXTS[]`, plus the JSON-schema
  interior light). **To add/change an HA entity, edit the relevant table** —
  discovery + state publish + command routing all derive from it.
- `discovery_prefix` = `homeassistant` (HA default).

## Hardware / pin map (unchanged from ESPHome, defined in config.h)

| Function           | GPIO | Notes |
|--------------------|------|-------|
| OneWire upper      | 22   | 1 DS18B20 |
| OneWire lower      | 26   | 1 DS18B20 |
| OneWire compressor | 4    | 1 DS18B20 |
| OneWire ambient    | 33   | **two** sensors (ambient + freezer) matched by ROM addr |
| CT clamp ADC       | 34   | ADC1_CH6, 12 dB atten, 200 ms RMS |
| Compressor relay   | 5    | active-high |
| Fan relay          | 18   | active-high |
| Buzzer (K2)        | 19   | active-high |
| K1 spare relay     | 21   | active-high, HA-exposed |
| Door sensor        | 39   | HIGH = open (fail-safe open on wire break); input-only pin |
| Settings button    | 35   | active-low, external 4k7 pull-up; input-only pin |
| Panel LED 1/2/3    | 13/32/14 | **active-low** sink drivers |
| Status LED         | 25   | onboard |
| Interior light     | 15   | LEDC PWM 1 kHz, 10-bit |

## Calibration — CONFIRMED CORRECT, do not change blindly

The user confirms the ESPHome build ran for a week+ with **correct DS18B20
addresses and correct CT current**. So the crashes were NOT a config problem
(see "Root cause" below). Keep these as-is:

- **DS18B20 addresses** (`ADDR_AMBIENT` / `ADDR_FREEZER` in config.h) are known
  good — carried over verbatim from the working ESPHome config. Don't "fix" them.
- **CT clamp** (`CT_CAL_SLOPE` / `CT_CAL_OFFSET`) is the least-squares fit of the
  three working ESPHome `calibrate_linear` points (0→0, 0.04303→1.17,
  0.2912→7.94); `CT_NOISE_FLOOR_A` = 0.30 A. One caveat to verify on hardware:
  the native ADC-oneshot RMS path is not bit-identical to ESPHome's `ct_clamp`
  component, so do a *sanity* check against a clamp meter. If it's off it's a
  scale tweak, not a rewrite — the underlying calibration is correct.

## Root cause of the crashes (what we're actually fixing)

The ESPHome build **worked for a week+ but crashed often** — "99% timeouts, then
watchdog kills everything." Not a wiring/config/calibration issue. It was the
cooperative single-loop runtime: intermittently something blocked the loop long
enough to trip the watchdog. The known blockers in that config were the CT-clamp
200 ms blocking RMS sample and the four bit-banged OneWire transactions (each
masks interrupts in short slots), all sharing the loop with WiFi/API reconnects.
The native rewrite removes this class of failure by construction: those blocking
operations now live in separate FreeRTOS tasks, and the WDT-subscribed control
task never blocks. **Success metric for this migration = long uptime with zero
Task-WDT resets**, not any behavioural change. (Check "Reset Reason" entity /
serial log for `Task WDT` after it's been running.)

## Known intentional differences from ESPHome

- **ESP32 internal die-temperature sensor was dropped.** The IDF
  `temperature_sensor` driver macro (`TEMPERATURE_SENSOR_CLK_SRC_DEFAULT`)
  didn't resolve on the classic-ESP32 target in this framework-espidf version,
  and it's only a diagnostic. `sensors_internal_temp()` returns NAN; the
  "ESP32 Internal Temp" HA entity was removed. Re-add later if desired by
  finding the correct driver include/clk source for this IDF.
- Diagnostic "Loop Time" sensor (ESPHome `debug` platform) is not reproduced;
  not meaningful in the multi-task model. Heap Free / Heap Max Block are kept.

## Behavioural features ported (all from fridgectl.yaml)

Compressor control with hysteresis + 3 min min-off, turbo (30 min), vacation
mode, blind duty cycle (both chamber sensors dead), defrost (auto by compressor
runtime + manual), fan run-on + door cutoff, buzzer alarms with 10 min silence,
overcurrent latch, compressor overtemp latch, CT/capacitor fault detection,
food-safety timers (fridge + freezer), freezer warm/cold/critical latched
alarms with hysteresis, duty-cycle EMA, cooling efficiency, inrush peak capture,
temp rate-of-change anomaly, 3-LED status UI, button short/double/long actions,
interior light follows physical door. Persisted (NVS, deferred 5 min):
runtime/starts counters, duty EMA, efficiency, temp history, freezer latches,
all setpoints/config numbers, system_enable/door_override/vacation switches.

**Beyond the spec: chamber hard floor (2026-10-13).** The compressor is locked
out while `t_ctrl` < max(sp − 0.5 − 2.0, −1.0) °C and released 1 °C above that
(`CHAMBER_FLOOR_*` in config.h). It shows as alarm `CHAMBER TOO COLD`, binary
sensor `chamber_too_cold`, and LED3 slow blink. It is a backstop against any
demand-logic bug, such as the turbo wraparound that ran the compressor nonstop
to −7 °C. Normal cycles never reach it, so it only trips when something forces
cooling past the setpoint.

## Subsystem notes (lazily loaded)

Per-subsystem design rationale, the post-migration feature history, and the
on-hardware verification log live in the **`fridgectl-subsystems` skill**
(`.claude/skills/fridgectl-subsystems/SKILL.md`) so they don't sit in context
every session. Read it before changing OTA, the 1-Wire scan/ROM assignment,
the log ring, alarm states, or the LED/button/beeper behaviour.

## Status & next steps (as of 2026-06)

- ✅ Full migration **flashed and running on real hardware** at `192.168.1.4`.
  OTA via web page confirmed working. Build clean (Flash 57.1%, RAM 13.2%).
- ✅ `secrets.h` filled in by user (WiFi + MQTT credentials; see `secrets.h.example`).
- ✅ Post-migration features added (see section above): OTA rollback (heartbeat-
  based, offline-safe), INITIAL COOLDOWN / SYSTEM OFF states, physical on/off
  combo, light dim-when-off + 1 s fade, two-stage web UI.
- ✅ **Migration goal met: zero watchdog resets in long-uptime soak** (190k+ s /
  >2 days continuous, "Reset Reason" never `Task WDT`). The remaining "Other WDT"
  (hardware, fan inductive kickback) was **fixed with an RC snubber on the fan
  relay** — see the diag black-box section. Both watchdog classes now resolved.
- ✅ Published to GitHub (public) under **GPLv3** (`LICENSE`).
- ✅ **2026-10-13: 24.8-day timer wraparound fixed + chamber hard floor added.**
  The first uptime past 2³¹ ms (2026-10-12) latched turbo on and over-cooled
  the chamber to −7 °C. Both fixes were OTA'd and verified on hardware with a
  clock-offset test image (details in the subsystems skill). Lesson: long uptime
  is what exposed it, so any `*_until` timer is suspect until it has run past
  24.8 days.
- 🔎 CT current still worth a one-time clamp-meter sanity check (native ADC RMS
  path isn't bit-identical to ESPHome's `ct_clamp`); addresses/CT are confirmed
  good otherwise — don't retune blindly.
- 🧊 Chamber/freezer DS18B20 probes to be moved into a **propylene-glycol buffer**
  (glycol on order) — air-mount works but is twitchy; see README/MANUAL. Defrost
  is run-time based so buffering won't affect it.

