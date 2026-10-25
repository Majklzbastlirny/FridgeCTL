---
name: fridgectl-subsystems
description: Design rationale and implementation notes for FridgeCTL's post-migration subsystems - staged OTA shutdown and rollback health check, the 1-Wire bus scan and freezer/ambient ROM assignment, the in-RAM log ring behind /api/logs, alarm states (INITIAL COOLDOWN / SYSTEM OFF / FIRMWARE UPDATE), the physical button combo, interior light behaviour, the beeper task, the reset-diagnostics black box, and the on-hardware verification log. Read this before modifying any of those subsystems.
---

# FridgeCTL subsystem notes

Moved out of the always-loaded `CLAUDE.md` so it loads only when relevant.
Safety rules stayed in `CLAUDE.md` (control-path blocking ban, calibration lock).

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

- **Deadline wraparound fix (2026-10-13).** Incident: after 24.8 days of
  uptime, turbo switched itself on (`history (79).csv`: uptime 2147483 s at
  01:38:18Z on 10-12, turbo on at the next publish). It ran the compressor
  nonstop for ~40 h, interrupted only by the 20 min auto-defrosts, with the
  chamber at −7 °C and the freezer at −30 °C. Cause: `*_until` fields used 0 for
  "off" plus `(int32_t)(until - now) > 0`, which is true for 0 once `millis()`
  > 2³¹. Same window: buzzer muted, panel LEDs stuck on the setpoint display.
  Inherited verbatim from the YAML. Fixed with `deadline_pending()` /
  `deadline_after()` (app_state.h) plus `expire_deadlines()` at the top of
  `control_tick()` (before *every* early return). Without that sweep, a stale
  deadline still flips 24.8 days after it expired. `light_off_at` was already
  safe (guarded and self-clearing). The uptime sensor now uses the 64-bit
  `esp_timer`. Test hook: `-DFRIDGECTL_CLOCK_OFFSET_MS=2147183648u` via
  `PLATFORMIO_BUILD_FLAGS` boots 5 min short of the crossing; boot log prints
  `TEST BUILD: millis() offset … now <value>`, and that value is the proof
  the offset applied.
- **Chamber hard floor (2026-10-13, beyond the spec).** See CLAUDE.md. Computed
  from the *effective* setpoint (vacation-aware) in `control_tick()`, applied
  as the first safety override. The alarm ranks just below the chamber-sensor
  faults. No buzzer, on purpose: it is self-correcting. After any long-off or
  runaway episode, expect `CHAMBER TOO COLD` right after boot until the chamber
  warms past floor + 1 °C. That is correct, not a fault.

- **Cold-room freezer support (2026-10-23).** Evidence from `history (85).csv`
  (9 days after the wraparound fix, 10 days uptime, zero resets):
  - Ambient was ~12 °C and duty 16.5%: 5 runs a day of ~48 min, ~4 h off.
  - Freezer −1.8 °C/h while running and +0.34 °C/h while off, settling near
    −11.7 °C, so `FREEZER TOO WARM` was on 89% of the time.
  - Post-incident recovery: zero compressor runs from 10-13 18:09 to 10-16
    09:48 while the over-cooled chamber drifted back up to the 5.5 °C cut-in.
    The freezer peaked at −3.2 °C. The floor was active only for the first
    ~10 h; the rest was normal dead-band "maintain off".

  A rough two-point heat model (wall leak k≈0.0147/h, evaporator side
  k≈0.075/h, Te≈−40 °C) says −15 °C needs ~22% duty with the fridge at ~3 °C,
  and −18 °C needs ~27% with the fridge at ~1–2 °C. In an 8 °C room, plain
  control could let the freezer drift toward ~−5 °C. These are estimates for
  orientation only.

  Implementation (`control_tick`):
  - `floor_c` is now computed before demand (from the user setpoint).
  - Assist state has ±`FREEZER_ASSIST_HYST_C` hysteresis around
    `freezer_target`. It goes inactive if the freezer probe is
    invalid/disabled, in blind mode, or when the system is off.
  - `sp_ctl` replaces `setpoint` *only* in the two demand comparisons and the
    Compressor ON/OFF log lines, which print the effective setpoint and an
    `ASSIST` tag.
  - The offset is clamped to
    `(sp − HYST_BELOW) − (floor + FREEZER_ASSIST_FLOOR_GAP_C)`: 1.0 at sp ≥ 1.5
    and smaller below that, where the floor's −1 °C clamp bites.
  - Fan Inhibit is checked after the door cut-off and skipped during defrost.
  - Defrost credit lives in the compressor-off branch of the runtime counters
    and is skipped while `defrost_active`.

  Entities: switches `freezer_assist` and `fan_inhibit`, numbers
  `freezer_target` and `assist_offset`, binary sensor `freezer_assist_active`.
  NVS keys: `frz_assist`, `fan_inhibit`, `frz_target`, `asst_offset`.

## Hardware-verified (2026-10-23, device at 192.168.1.75)

- Cold-room build (isolated PIO 6.1.19, pioarduino 55.03.39 / IDF 5.5.4,
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` still present) OTA'd to **both**
  slots back to back. Each passed its health check at ~38 s; the second went in
  inside the 3-min post-boot compressor lockout, so the run was cut only once.
  New entities present with the expected defaults (assist off, target −18,
  offset 1.0).
- **Fan Inhibit switched ON at 19:37Z** to start the 2-day test. Baseline
  (previous 9 days, ~12 °C room): 48-min runs, ~16.5% duty, freezer ~−11.7 °C.
  It works if runs get longer than ~48 min, duty rises above ~20%, and the
  freezer trends below −13 °C while the chamber holds ~4 °C. Check
  upper/lower spread, and compare against ambient (the room is cooling at the
  same time).
- **Fan test result (`history (2).csv`, 47.4 h inhibited vs 21.6 h before,
  2026-10-25): break-even.** Fan Inhibit lengthens the cycles but doesn't add
  runtime. Runtime is set by the chamber's heat balance, and the fan only
  redistributes it.

  | | Fan normal | Fan inhibited |
  |---|---|---|
  | Run length | 48 min | 70 min |
  | Off period | 255 min | 362 min |
  | Runs per day | ~5 | 3 |
  | Duty | 16.5% (prior-week baseline) | 14.8% (~6% of the drop is the 0.4 °C cooler room) |
  | Freezer drop per run | −1.35 °C | −2.7 °C |
  | Freezer cooling per run-minute | −1.64 °C/h | −2.32 °C/h |
  | Freezer mean | −11.56 °C | −11.72 °C |
  | Freezer swing | 2.8 °C | 4.2 °C |

  Faster freezer cooling per run-minute means less fridge-plate load lets the
  evaporator run colder. But the off periods stretched just as much: the cold
  stored in the wall bleeds out slowly, and the near-wall probes see it.
  Chamber mean was unchanged (4.47 → 4.44), and the upper/lower spread
  *shrank* (0.87 → 0.61), the opposite of the stratification worry. Kept ON
  for fewer starts and no downside beyond the wider freezer swing.
- **Natural defrost credit verified:** `Defrost counter reset after 60 min
  off (natural defrost)` appears once per cycle (4× in the log tail), and
  there was no forced defrost in 47 h.
- Freezer peaks drifted −10.1 → −9.8 °C and run-end minima −12.9 → −12.6 °C
  over the 2 days, tracking the room cooling from 11.9 to 10.8 °C.
- **Freezer Assist switched ON at 2026-10-25 19:14Z** (Fan Inhibit stays ON;
  the 47 h above are the baseline). It engaged immediately:
  `Freezer assist ON (freezer -9.7 C)` then `Compressor ON (t=5.4 sp=3.0
  ASSIST)`, starting at the new 4.5 °C cut-in. Rough expectation at a ~11 °C
  room: fridge mean ~3.4 °C, duty ~17%, freezer ~−13 °C.

## Hardware-verified (2026-10-13, device at 192.168.1.75)

- Test image (clock offset 2³¹ − 5 min) OTA'd: boot log `now 2147183664`,
  health check PASSED at 38.2 s. Polled every 5 s through uptime 300 s
  (crossing at 18:48:20Z): turbo stayed `false` and the compressor stayed off.
  After the crossing, turbo ON → reads `true` and OFF → reads `false` over
  `/api/cmd`, which the old firmware could not do.
- Production OTA'd: no `TEST BUILD` line, health check PASSED (38.2 s), and
  System Enable came back on (persisted). The floor tripped on the first tick
  (`CHAMBER FLOOR: -4.0 C below 1.5 C`), alarm `CHAMBER TOO COLD`, binary on.
  Turbo switched on while tripped: compressor stayed off.
- Natural expiry (the `expire_deadlines()` sweep): turbo was switched on at
  80087 ms of uptime and the log showed `Turbo expired` at 1882506 ms, i.e.
  30 min 2.4 s (within one 5 s tick). HA showed it off, compressor never ran.

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

