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
  `192.168.1.4` (DHCP) and has been flashed + OTA-tested on real hardware.
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

- Windows 11, PowerShell default shell. There are **two PlatformIO cores**
  installed (6.1.15 and 6.1.19) — harmless warning, ignore it. `pio` works.
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

## Layout

```
fridgectl.yaml        ORIGINAL ESPHome config = behavioural spec, keep for reference
platformio.ini        env:fridgectl, framework=espidf
partitions.csv        2x OTA app slots
sdkconfig.defaults    Task WDT 10s, Int WDT 800ms, 4MB flash, FreeRTOS 1000Hz
CMakeLists.txt        IDF project root
src/
  secrets.h           REAL credentials, git-ignored (user has filled it in)
  secrets.h.example   template
  config.h            ALL pins, thresholds, CT calibration  ← tune here
  app_state.{h,cpp}   single shared AppState struct g + recursive mutex; millis()
  nvs_store.{h,cpp}   persistence; deferred commit every 5 min (flash wear)
  ow.{h,cpp}          bit-banged 1-Wire + DS18B20 (own driver, no RMT components)
                      incl. ow_search() = Maxim SEARCH ROM 0xF0
  sensors.{h,cpp}     temp_task (DS18B20 + bus scan) + ct_task (ADC RMS + inrush)
  logbuf.{h,cpp}      esp_log_set_vprintf hook -> 120-line RAM ring for /api/logs
  gpio_io.{h,cpp}     relays / LEDs / light PWM / input_task (door + button)
  control.{h,cpp}     the 5 s control loop + derived telemetry + button handlers
  leds.{h,cpp}        500 ms LED/UI loop + status LED
  wifi_net.{h,cpp}    STA + always-on fallback AP (APSTA)
  mqtt_net.{h,cpp}    esp-mqtt + HA MQTT Discovery + command routing (BIGGEST file)
  ota.{h,cpp}         HTTP firmware-upload endpoint
  main.cpp            init order + task creation
```

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
matching Arduino semantics the ESPHome logic assumed).

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

## Post-migration additions (native firmware only, NOT in fridgectl.yaml)

- **INITIAL COOLDOWN alarm state + binary sensor.** Non-persisted
  `g.reached_setpoint` (false each boot) latches true once `t_ctrl` first enters
  the band (`<= setpoint + HYST_ABOVE_C`). Until then Alarm State reads
  `INITIAL COOLDOWN` = "powered up from room temp, not ready yet". Re-armed
  whenever the system is disabled (chamber warms while off). Blind mode counts
  as reached (don't mask BLIND). Suppressed when `!system_enable`.
- **SYSTEM OFF alarm state.** `ctl_alarm_state()` returns `SYSTEM OFF` when
  `!system_enable` (it returns early *before* the cooldown/fault checks). Fixes a
  bug where a disabled system still showed INITIAL COOLDOWN. `ctl_initial_cooldown()`
  also returns false when disabled. (control_tick already returns early when off.)
- **Physical on/off combo.** Hold the settings button AND open the door
  `COMBO_DOOR_OPENS` (3) times within `COMBO_WINDOW_MS` (5 s) → toggles
  `system_enable` both directions (`on_combo_toggle_system`, buzzer+LED feedback).
  Deliberate two-input gesture so a single stuck button / flapping door can't
  trigger it. Detected in `gpio_io.cpp` input_task; turbo long-press is suppressed
  when the door was used during the hold (`door_during_hold`).
- **Interior light dims to 25% when system is OFF** (full 100% when on) — opening
  the door signals the off-state at a glance. Logic in both `on_door_change`
  (control.cpp) and `update_interior_light` (leds.cpp).
- **Audible on/off feedback via the piezo** (self-oscillating, on relay K2).
  Central `beeper_task` (control.cpp) watches `system_enable` for an edge and
  chirps regardless of source (combo / HA / web): ON = two short chirps, OFF =
  one long chirp. It shares K2 with the alarm buzzer — it only pulses briefly
  and resets `g.k2_on`, so the next 5 s control tick re-asserts any real alarm
  (worst case a real buzzer is delayed one tick). The combo handler no longer
  beeps inline (it only LED-flashes); all beeping is centralized here.
- **Staged OTA shutdown (avoids the mid-flash fan transient).** Diagnosed from
  the black-box trace: a `WDT` reset happened when the OTA forced compressor+fan
  off in the same instant the flash write (cache off, CPU stalled) was running —
  the fan's inductive turn-off transient crashed the chip before `esp_restart()`
  could stamp the SW hint (hence WDT, not SW). Fix in `update_post`: Phase 1 =
  compressor off but **fan kept running**; Phase 2 = receive + flash the image
  (fan still on); Phase 3 = flash done → fan off → settle `OTA_FAN_SETTLE_MS`
  (3 s) so the transient decays → `esp_restart()`. Also `control_tick` now
  **freezes all relay handling while `ota_in_progress`** (returns early, only
  bumps heartbeat) so the 5 s loop can't toggle the fan mid-flash. Web page
  shows "Firmware received… rebooting, reconnecting in Ns" countdown then
  auto-reloads.
- **OTA reflected in Alarm State + entities + beep.** `g.ota_in_progress` /
  `g.ota_failed` (app_state.h, not persisted). During an upload Alarm State =
  `FIRMWARE UPDATE` (high priority, right after BOOTING). On any abort (bad
  image, dropped connection, ota_end CRC) `ota_fail()` in ota.cpp latches
  `FIRMWARE UPDATE FAILED` (ranked among degraded warnings; clears on next
  successful OTA or reboot). Two HA binary sensors: `ota_in_progress`,
  `ota_failed`. The beeper task also plays a distinct **3 long descending
  chirps** on the ota_failed rising edge. Note: a successful OTA reboots, so
  the in-progress state is naturally transient; rollback (unhealthy image)
  still just reboots into the old slot and shows up via Reset Reason, not a
  dedicated alarm.
- **Interior light 1 s fade** in/out via hardware LEDC fade (`ledc_set_fade_with_time`
  + `ledc_fade_start(LEDC_FADE_NO_WAIT)`, non-blocking; service installed in
  `gpio_io_init`). `LIGHT_FADE_MS` in config.h.
- **`g.control_beats`** heartbeat counter (++ every control tick, both enabled and
  disabled branches) — liveness proof for the OTA rollback health check.
- **Two-stage web UI** (see "Web UI" section above).
- **Reset diagnostics black-box (diag.{h,cpp}).** Added to investigate an
  intermittent `ESP_RST_WDT` ("Other WDT") reset that also occurred under the
  old ESPHome build → suspected HARDWARE, not firmware (persists across two
  firmwares). User's `history (40).csv` showed resets tend to follow the **fan
  turning off** → classic inductive-kickback / relay-arc EMI on an unsnubbed
  motor relay. Two tools: (1) breadcrumb ring in `RTC_NOINIT_ATTR` memory
  (survives WDT/brownout reset, not full power loss) recording the last 12
  relay/system events (COMP±/FAN±/DEF±/SYS±/OTA/BOOT) with timestamps —
  snapshotted at boot into "Pre-Reset Trace" so you can see what immediately
  preceded a reset; (2) persistent per-reason reset counters in NVS namespace
  "diag" → "Reset Counts" (PO/SW/PANIC/INT/TASK/WDT/BO). Both new HA text
  sensors. `diag_init()` runs right after `nvs_store_init()` in main.cpp;
  `diag_event()` is called at each relay transition. NOTE: ESP_RST_WDT is the
  RTC/timer-group WDT, NOT the Task WDT the migration fixed — getting "Other
  WDT" (not "Task WDT") is consistent with the migration working. Likely
  hardware fix: RC snubber across the fan relay contacts / flyback handling,
  and/or bulk capacitance on the 5V rail.
  **✅ CONFIRMED FIXED (2026-06): user fitted an RC snubber on the fan relay and
  the "Other WDT" resets stopped completely — 190k+ s (>2 days) continuous
  uptime with zero crashes.** So the two watchdog classes had two distinct
  fixes: the migration eliminated the **Task WDT** (cooperative-loop starvation)
  by construction; the **snubber** eliminated the **Other WDT** (fan inductive
  kickback / relay-arc EMI). The diag black-box (Pre-Reset Trace showing `FAN-`
  before each reset) is what pinpointed the fan. Both were needed — a snubber
  alone wouldn't have saved the old ESPHome build's Task-WDT starvation.
- **DS18B20 read robustness (intermittent sensor faults).** Symptom: occasional
  one-cycle "sensor fault" (lower/compressor) that self-clears next refresh —
  classic single dropped OneWire transaction (noise, marginal slot). Two layers
  fix it: (1) `ds18b20_read_single/addr` in ow.cpp now **retry up to 3×** on
  NAN (no-presence or CRC fail) — the scratchpad already holds the conversion
  so retries are cheap; (2) `temp_task` (sensors.cpp) **debounces** via
  `commit()` — a NAN only overwrites the published value after `MISS_LIMIT` (3)
  consecutive failed cycles, holding the last good value meanwhile. A real dead
  sensor still trips within ~1.5 min; a one-off glitch is now invisible.
- **`freezer_sensor_enable` switch (persisted, default ON).** Lets the user
  cleanly disable the freezer probe when it's removed/disconnected (the cable was
  breaking the freezer door gasket → frost crust). When OFF: `control_tick`
  skips the freezer monitoring block **and clears any armed/latched freezer
  alarms**, and `ctl_freezer_sensor_fault()` returns false (no permanent fault
  binary). Wired as a normal `SWITCHES[]` entry (`"freezer_sensor"`,
  `mdi:thermometer-off`) → auto HA discovery + web control + command routing;
  persisted in NVS as `frz_sens_en`. Freezer probe is on the shared ambient bus
  (by ROM addr), so disabling/removing it doesn't affect the ambient sensor.
- **Per-sensor temperature calibration offsets (5× persisted NUMBERs, default 0).**
  Each DS18B20 reading gets a `±5 °C` trim added before publishing. Fields
  `g.off_upper/off_lower/off_compressor/off_ambient/off_freezer` (app_state.h),
  applied in `sensors.cpp` `commit()` — the offset is added to a *valid* reading
  only (a NAN/fault is never shifted), under the state lock, so every downstream
  consumer (control, MQTT, web) sees the corrected value. Exposed as five
  `NUMBERS[]` entries (`off_upper`…`off_freezer`, step 0.1, `cfg=true`) via the
  `NSET` macro → auto HA discovery + web control + command routing; persisted in
  NVS as `off_upper/off_lower/off_comp/off_amb/off_frz`. Motivation: a stabilised
  all-at-ambient soak (`history (49).csv`) showed a ~1.7 °C spread between probes
  that was constant across every time window (= genuine fixed offset, not drift).
  With no truth sensor, estimate offsets relative to the 5-sensor group mean
  (≈17.1 °C in that soak): freezer −0.9 and ambient +0.8 were the clear outliers;
  compressor was the group anchor (~0). User sets the actual values in HA/web.
  Note: bumped the web `/api/state` JSON buffer in `ota.cpp` 6144→8192 to keep
  headroom after adding 5 entities (silent snprintf truncation would corrupt the
  whole control page).

- **1-Wire sensor browser + runtime freezer/ambient ROM assignment.** The shared
  GPIO33 bus used to be opaque: `ADDR_AMBIENT`/`ADDR_FREEZER` were compile-time
  constants and swapping probes meant edit-recompile-reflash. Now:
  `ow_search()` (ow.cpp) implements **Maxim SEARCH ROM 0xF0** on top of the
  existing private `ow_read_bit`/`ow_write_bit`, accumulating `rom |= 1ULL<<bit`
  — which is exactly the LSB-first/family-code-low encoding `read_addr_once`
  puts on the wire and that the `config.h` literals use, so **no byte swapping
  anywhere**. Results are CRC-checked and filtered to family `0x28`.
  `temp_task` owns the scan (`do_scan()` in sensors.cpp) — the bit-banged bus
  has **no task-level mutex** (`s_ow_mux` is a per-slot interrupt lock only), so
  the HTTP handler must never touch it; it only raises `s_scan_req`. The 30 s
  tail delay is waited in 1 s slices so a web-triggered rescan starts in ≤1 s.
  Roles are `g.addr_ambient`/`g.addr_freezer` (`uint64_t`, **0 = use the
  config.h default**, which keeps the known-good values as the fallback and
  avoids pulling config.h into app_state.h); persisted as NVS `addr_amb`/
  `addr_frz`. Assignment goes through `type:"rom"` in `cmd_post` → `assign_rom()`,
  which **rejects any ROM the last scan didn't see on the shared bus** and, when
  you claim the probe the other role owns, hands that role yours — so a swap is
  one click. It calls `nvs_store_save()` **directly** (not `mark_dirty`): the
  deferred committer is 5 min and a reboot must not lose a deliberate change.
  `sensors_roles_changed()` then clears the `miss_a`/`miss_f` debounce so
  `commit()` can't keep publishing a held value belonging to the *other* probe.
  Calibration offsets deliberately stay with the **role**, not the probe.
  Exposed as `GET /api/onewire` + a control-page section, plus an `ow_scan`
  entry in `BUTTONS[]` (so HA gets a "Scan 1-Wire Buses" button for free).
  `temp` task stack 4096→5120 (the scan builds an `OwFound[]` snapshot on it).
- **Raw log viewer (`logbuf.{h,cpp}` + `GET /api/logs`).** `logbuf_init()` is the
  **first statement in `app_main`** (before the boot banner, before
  `state_init_mutex()`) and installs an `esp_log_set_vprintf()` hook that chains
  to the previous handler — **serial output is unchanged**. 120 × 160 B static
  ring (~19 KB DRAM; static on purpose so it doesn't eat the heap headroom WiFi
  and OTA need). The hook runs on whatever task logged (and IDF keeps the log
  path in IRAM, so possibly ISR context): it formats on the **caller's stack
  outside any lock** and takes a dedicated `portMUX_TYPE` spinlock only for the
  ≤160 B copy. It must **never** take `StateGuard` — `state_lock()` waits
  forever and would invert priority against the WDT-subscribed control task.
  **One vprintf call ≠ one log line**: the WiFi blob emits its header and body
  as two separate calls (serial shows them on one line). So the hook accumulates
  into `s_pend` and only commits a ring entry on `'\n'`, reproducing the serial
  console exactly. (First version split them; fixed and verified on hardware —
  `I (603) wifi:wifi driver task: …` now arrives as a single line.)
  `/api/logs?since=N` streams via `httpd_resp_send_chunk` (the codebase's first
  chunked responder) and ends with a `#seq N` **trailer** — written last so
  lines arriving mid-response aren't skipped. Auth-gated: log lines carry SSID/
  IP/OTA detail. Timestamps are IDF's own `(ms)` prefix
  (`CONFIG_LOG_TIMESTAMP_SOURCE_RTOS`); `CONFIG_LOG_COLORS` is off so lines are
  clean ASCII. Note `CONFIG_LOG_MAXIMUM_LEVEL=3` compiles DEBUG out — bump
  `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` if more verbosity is ever wanted.
  Browser Copy uses a `<textarea>` + `execCommand` fallback because plain
  `http://` is not a secure context, so `navigator.clipboard` is undefined there.

- **Freezer alarm timers disarm when the probe is invalid (2026-08-06 fix).**
  All freezer arming/latching/clearing lived inside the `else if (valid
  reading)` branch of `control_tick`, so an invalid probe froze the whole block
  — including the arming timestamps `freezer_warm_ms` / `freezer_cold_ms` /
  `freezer_unsafe_ms`, which hold absolute `millis()` values. The consequence
  was **a false latch on reconnect**: pull the probe with a timer already armed,
  leave it out longer than the latch window, plug it back in, and the very first
  out-of-range sample satisfies `now - freezer_*_ms > THRESHOLD` and latches
  instantly — having never observed the sustained condition the latch is
  supposed to prove. With `FREEZER_UNSAFE_MS` = 2 h, a 2-hour rewire followed by
  one warm reading would latch **FREEZER FOOD SAFETY** on no evidence at all.
  Fixed with a new `else` branch that zeroes the three timers whenever the probe
  is enabled but unreadable, so a returning probe must re-accumulate the full
  duration. Existing latches are deliberately **not** cleared there — they
  record that the condition really happened, and the hysteresis branches already
  clear them once the probe reads normally again.
  `ctl_alarm_state()` also now returns **`FREEZER SENSOR FAULT`** (ranked just
  above the warm/cold latches, but *below* `FREEZER FOOD SAFETY`) so a missing
  probe reads honestly instead of showing a stale "FREEZER TOO WARM".
  Note the `freezer_sensor` switch OFF branch still takes priority and clears
  everything, so the switch remains the clean way to park a removed probe.

## Hardware-verified (2026-08-06, device at 192.168.1.4)

Both new features were OTA-flashed and exercised on the real fridge:
- `1-Wire scan: 5 device(s) found across 4 buses`. The two shared-bus ROMs read
  back as `BD0623C34C9E4A28` / `4E00001096F82428` — **character-identical to
  `config.h`**, proving the SEARCH ROM bit encoding needs no byte swapping.
  Newly visible: upper `940000109875B728`, lower `C500001098394228`,
  compressor `F16FE700879C5128`.
- One-click ambient↔freezer swap works, the published temperatures crossed over
  on the next ambient cycle, and — importantly — both went **unavailable first**
  rather than holding a stale value, which is `sensors_roles_changed()` clearing
  the `miss_a`/`miss_f` debounce as intended. The swap **survived a reboot**
  (direct `nvs_store_save()`), then was restored to the config.h mapping.
- Rejections behave: a ROM from a dedicated bus, an unknown ROM, and a bad role
  name are all refused with a message.
- Log tail matches serial; `?since=` incremental fetch returned 66 bytes vs 3328
  for the full dump.
- Post-flash: alarm `OK`, no active fault flags, `TASK:0` still (no Task-WDT
  resets). `SW` count rose only by the reboots deliberately triggered here.

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
- 🔎 CT current still worth a one-time clamp-meter sanity check (native ADC RMS
  path isn't bit-identical to ESPHome's `ct_clamp`); addresses/CT are confirmed
  good otherwise — don't retune blindly.
- 🧊 Chamber/freezer DS18B20 probes to be moved into a **propylene-glycol buffer**
  (glycol on order) — air-mount works but is twitchy; see README/MANUAL. Defrost
  is run-time based so buffering won't affect it.
