# FridgeCTL — User Manual

Smart fridge controller for the LilyGo T-Relay (ESP32).

---

## Table of contents

1. [General information](#1-general-information)
2. [Simple usage guide](#2-simple-usage-guide)
3. [Advanced](#3-advanced)
4. [Specifications & pin map](#4-specifications--pin-map)

---

## 1. General information

### What it does

FridgeCTL turns a basic fridge/freezer into a smart, self-protecting appliance.
It measures temperatures, drives the compressor with proper hysteresis and
safety timers, runs the fan, watches the compressor's electrical current, and
raises alarms when something is wrong. It can run **completely on its own** with
no network — Wi-Fi and Home Assistant are optional conveniences, not
requirements.

### What it controls and senses

- **Compressor** — switched on/off to hold your target temperature.
- **Fan** — runs with the compressor (plus a short run-on afterward).
- **Buzzer (piezo)** — audible alarms and confirmation beeps.
- **Interior light** — follows the door, fades smoothly in and out.
- **3 panel LEDs** — at-a-glance status (system / cooling / alarm).
- **Temperature sensors** — fridge upper & lower, compressor, ambient, freezer.
- **Current sensor (CT clamp)** — watches the compressor's power draw to detect
  faults (overcurrent, no-current, failing start capacitor).
- **Door sensor** and a **settings button** for local control.

### Key safety behaviors (always active)

- **Minimum off-time** (3 minutes) — protects the compressor from rapid
  restarts.
- **Overcurrent and over-temperature cut-offs** — shut the compressor down and
  latch an alarm if it draws too much current or runs too hot.
- **Door cut-offs** — fan stops if the door is open over 1 minute; compressor
  stops if the door is open over 5 minutes.
- **Food-safety timers** — alert if the fridge stays too warm, or the freezer
  drifts out of range, for too long.
- **Blind duty cycle** — if both fridge sensors fail, it falls back to timed
  on/off cooling so food stays cold.

### Settings remembered across power loss

Your target temperature, all configuration values, and the on/off state are
saved automatically and restored after a power cut.

---

## 2. Simple usage guide

Everything in this section is done with the **physical button** and **door** —
no phone or computer needed.

### The button (single settings button on the panel)

| Action | What it does |
|---|---|
| **Short press** | Steps the target temperature: 2 °C → 4 °C → 6 °C → 8 °C → back to 2 °C. The panel LEDs briefly show the chosen level (see below). |
| **Double press** | Toggles **Door Override** on/off (tells the system to ignore the door sensor — useful if the door switch is faulty). The light still works normally. |
| **Long press (hold ~3 s)** | Toggles **Turbo** mode: 30 minutes of forced maximum cooling. The panel triple-flashes to confirm. |
| **Any press** | Silences an active alarm buzzer for 10 minutes. |

### Reading the panel temperature display

Right after a short press, the three LEDs show the new target for a few seconds:

| Target | LEDs |
|---|---|
| 2 °C | LED 1 |
| 4 °C | LEDs 1 + 2 |
| 6 °C | LEDs 1 + 2 + 3 |
| 8 °C | all three blinking together |

### Turning the whole system ON or OFF (the combo)

To switch the fridge controller fully on or off **without a phone**:

> **Hold the button down, and while holding it, open the door 3 times within 5 seconds.**

This deliberate two-step gesture exists so it can't happen by accident — a stuck
button or a flapping door alone will never trigger it. The same gesture turns it
back on.

You'll get feedback:
- **Turning ON** → two short beeps + LED flash.
- **Turning OFF** → one long beep + LED flash.

**How to tell it's OFF at a glance:** when the system is off, opening the door
lights the interior at **25% brightness** instead of full. Dim light = system
off.

### Reading the 3 panel LEDs (normal operation)

| LED | Meaning |
|---|---|
| **LED 1 — System** | Solid = healthy. Fast blink = a sensor problem or fault. Slow pulse (and others off) = system is OFF. |
| **LED 2 — Cooling** | Solid = compressor running. Fast blink = Turbo. Slow blink = Defrost. Off = idle. |
| **LED 3 — Alarm** | Off = all good. Slow blink = warning. Solid = critical alarm. |

### What the beeps mean

| Sound | Meaning |
|---|---|
| Two short beeps | System turned ON |
| One long beep | System turned OFF |
| Continuous / intermittent buzzing | An alarm is active (press the button to silence for 10 min) |
| Three long beeps | A firmware update failed (see Advanced) |

### Everyday tips

- **Normal target** for a fridge is around **4 °C**. Lower = colder.
- If you’re going away, you can raise the target (or use **Vacation Mode** from
  Home Assistant / the web page) to save energy.
- If the buzzer is sounding, **press the button** to silence it for 10 minutes
  while you investigate — silencing does not clear the underlying problem.
- After a power cut the system waits about 30 seconds (and enforces the 3-minute
  compressor off-time) before cooling resumes. This is normal.

---

## 3. Advanced

This section covers the network features: the web interface, Home Assistant, and
firmware updates.

### Finding the device

- On your network the controller currently appears at **`192.168.1.4`**
  (assigned by your router; it may change — check your router's device list, or
  the **IP Address** entity in Home Assistant).
- If it can't join your Wi-Fi, it always broadcasts its own fallback hotspot
  **"Fridgectl Fallback Hotspot"**. Connect to that and browse to
  **`http://192.168.4.1/`** to reach it for recovery.

### Web interface (two pages)

Open a browser to the device's address.

**Read-only dashboard — `http://<device-ip>/`**
- No password.
- Live temperatures and metrics, all status/fault indicators, the current
  **Alarm State**, and system info. Refreshes automatically every few seconds.
- Safe to give to anyone — nothing here can change settings.

**Control page — `http://<device-ip>/control`**
- **Password protected** (username `admin` by default; password is the one set
  when the firmware was built).
- Full control: every switch (System Enable, Turbo, Vacation, Door Override,
  Freezer Sensor, K1 spare), every setpoint and configuration value, action
  buttons (Reset Overcurrent, Trigger Defrost, Restart), and **firmware upload**.

> **Freezer Sensor** switch: turn this **off** if you remove or disconnect the
> freezer probe. It suppresses the freezer fault and all freezer alarms so a
> missing probe doesn't show a permanent fault. Turn it back on after the probe
> is reconnected.

### Home Assistant

If an MQTT broker is configured, the controller automatically appears in Home
Assistant as a single **FridgeCTL** device with all its sensors, switches,
numbers, and buttons — no manual configuration needed (it uses MQTT Discovery).
It also reports an **availability** status (online/offline). Everything you can
do on the web control page you can also do from Home Assistant.

### Configurable values (web control page or Home Assistant)

| Setting | Range | Purpose |
|---|---|---|
| Target Temperature | 1–10 °C | Normal fridge setpoint |
| Vacation Temperature | 5–12 °C | Used when Vacation Mode is on |
| Duty Cycle ON / OFF Minutes | 5–60 min | Timed cooling used only if both fridge sensors fail |
| Defrost Interval | 4–24 h | How much compressor run-time triggers an auto-defrost |
| Defrost Duration | 5–30 min | How long a defrost lasts |
| Freezer Warm / Cold / Critical Thresholds | various | When freezer alarms trigger |
| Upper / Lower / Compressor / Ambient / Freezer Temp Offset | −5…+5 °C | Per-sensor calibration trim added to that probe's reading |

### Calibrating the temperature sensors

Each DS18B20 has a small fixed error (the part is rated ±0.5 °C, and some are
worse). The five **Temp Offset** numbers let you trim each probe individually;
the offset is **added to the raw reading** before it is shown, alarmed on, and
used for control. Default is **0** (no correction).

To estimate offsets without a reference thermometer: switch the system **off**,
leave all probes in the **same still-air spot** for a few hours so they settle
to one true temperature, then read each value. Take the **average of the five**
as your best guess of the real temperature and set each probe's offset to
*(average − that probe's reading)*. A clamp/lab thermometer makes this exact —
then offset = *(reference − reading)*. Re-check after any probe is moved into a
glycol vial, since the thermal path changes slightly.

> The offsets only shift what the sensors report; they do **not** change any
> setpoint or threshold. Set offsets first, then tune Target Temperature.

### Alarm states (the "Alarm State" value)

Highest priority first. Most clear on their own once the cause is resolved;
some latch until acknowledged or until conditions return to normal.

| State | Meaning |
|---|---|
| `BOOTING` | Starting up / sensor warm-up. |
| `FIRMWARE UPDATE` | A firmware upload is in progress. |
| `SYSTEM OFF` | The system has been switched off. |
| `OVERCURRENT FAULT` | Compressor drew too much current — compressor latched off. Clear with **Reset Overcurrent Fault**. |
| `COMPRESSOR OVERTEMP` | Compressor too hot — off until it cools. |
| `BLIND DUTY CYCLE` | Both fridge sensors failed; running on timed cooling. |
| `UPPER / LOWER SENSOR FAULT` | One fridge sensor failed (still running on the other). |
| `DOOR OPEN` | Door left open too long. |
| `FOOD SAFETY ALERT` | Fridge stayed too warm for 2 hours. |
| `FREEZER FOOD SAFETY` | Freezer stayed above its critical limit for 2 hours. |
| `DEFROST` | A defrost cycle is running (compressor intentionally off). |
| `COMPRESSOR COOLDOWN` | Waiting out the 3-minute minimum off-time. |
| `COMP TEMP SENSOR FAULT` | Compressor temperature sensor unreadable. |
| `CT SENSOR FAULT` | No current detected while the compressor should be running. |
| `POSSIBLE CAP FAULT` | Intermittent current — possible failing start capacitor. |
| `FREEZER TOO WARM` / `FREEZER TOO COLD` | Freezer drifted out of range (latched). |
| `FIRMWARE UPDATE FAILED` | The last firmware upload was aborted. Clears on a successful update or reboot. |
| `INITIAL COOLDOWN` | Just powered up from a warm start — not yet down to temperature. Normal after first power-on. |
| `TEMP RISING WHILE COOLING` | Temperature climbing although the compressor is running — investigate. |
| `HIGH DUTY CYCLE` | Compressor running more than expected for the ambient temperature. |
| `OK` | Everything normal. |

### Firmware updates (OTA)

You can update the firmware over the network — no USB cable needed after the
first install.

**Using the web page (easiest):**
1. Go to `http://<device-ip>/control` and log in.
2. In the **Firmware** section, choose the new `firmware.bin` file.
3. Click **Upload & reboot**.

The controller switches all relays **off** before flashing, writes the new
image, and reboots into it. During this the Alarm State shows `FIRMWARE UPDATE`.

**If an update fails** (bad file, dropped connection): the controller keeps
running the **old** firmware, latches the `FIRMWARE UPDATE FAILED` alarm, and
**beeps three long times**. Nothing is broken — just try the upload again.

**Automatic rollback safety net:** a freshly uploaded firmware must prove itself
healthy (its control loop must actually start running) within about 90 seconds.
If it doesn't — for example it crashes on boot — the controller automatically
reverts to the previous working firmware on its own. Combined with the dual
firmware storage and the fallback hotspot, this makes a failed update very hard
to turn into a dead device.

### Recovery checklist

If the device seems unreachable or misbehaving:
1. Check your router for its current IP address.
2. If it's not on Wi-Fi, connect to **"Fridgectl Fallback Hotspot"** and open
   `http://192.168.4.1/`.
3. From the control page you can **Restart** it or re-upload firmware.
4. As a last resort, it can be re-flashed over USB.

### Notes

- The interior light fades over 1 second when it turns on or off, and shows at
  25% when the system is off (full brightness when on).
- Silencing the buzzer (button press) lasts 10 minutes and does **not** fix the
  cause of an alarm.
- The "Reset Reason" entity shows why the device last restarted — useful for
  diagnosing unexpected reboots.

---

## 4. Specifications & pin map

### Hardware

| Item | Detail |
|---|---|
| Controller board | LilyGo T-Relay (ESP32, classic dual-core) |
| Flash | 4 MB, dual firmware slots (for safe OTA + rollback) |
| Power | Per the T-Relay board (typically a 5 V supply) |
| Relays | 4 × on-board (Compressor, Fan, Buzzer/K2, Spare/K1) |
| Temperature sensors | 5 × DS18B20 (1-Wire): fridge upper, fridge lower, compressor, ambient, freezer |
| Current sensor | CT clamp on the compressor feed, read via ADC |
| Inputs | Door switch, settings push-button |
| Indicators | 3 × panel LED, 1 × onboard status LED, piezo buzzer, dimmable interior light |

### Temperature probe mounting (air vs. glycol buffer)

The DS18B20 probes are in sealed stainless shells, so you can mount them two ways:

- **In open air** — fast, reads air temperature directly, but twitchy: it spikes
  on every door opening and swings with each compressor cycle.
- **In a glycol buffer** (recommended for the two fridge-chamber probes) — seal
  the probe in a small vial of **propylene glycol** (food-safe; a ~30–50 % mix in
  distilled water, which won't freeze at fridge/freezer temperatures). The fluid
  is a thermal mass that mimics a small food item, so the reading reflects *food*
  temperature and rides smoothly through door openings and defrosts. This is the
  same "buffered probe" trick used in commercial and HACCP refrigeration.

| Probe | Mounting | Why |
|---|---|---|
| Fridge upper / lower | Glycol buffer | Smooth, stable control; reads food temp; no door-open nuisance |
| Freezer | Glycol buffer (smaller vial) | HACCP-correct; note it is slower to flag a genuine failure |
| Compressor | Clamped to compressor body (no buffer) | Over-temp protection needs a fast response |
| Ambient | Open air | Its job is to read room air |

**Does buffering affect defrost?** No. Defrost is triggered by accumulated
compressor run-time and ends on a fixed timer — it never looks at a temperature
reading, so damping the probes cannot change when or how long defrost runs.

> Use **propylene glycol** (also sold as *monopropylene glycol / MPG*), not the
> toxic *ethylene* glycol, and avoid ready-mixed automotive antifreeze (dyes and
> corrosion additives). A 1 : 1–2 mix with distilled water is ideal — the water
> improves thermal coupling and makes the buffer behave more like real food.

### Sensor & timing summary

| Parameter | Value |
|---|---|
| Temperature read cadence | ~30 s (ambient & freezer every other cycle, ~60 s) |
| Control loop | every 5 s |
| Compressor minimum off-time | 3 minutes |
| Cooling hysteresis | on at target +1.5 °C, off at target −0.5 °C |
| Fan run-on after compressor | 2 minutes |
| Door → fan cut-off | 1 minute open |
| Door → compressor cut-off | 5 minutes open |
| Turbo duration | 30 minutes |
| Buzzer silence duration | 10 minutes |
| Compressor over-temp cut-off | 75 °C (clears at 55 °C) |
| Overcurrent threshold | ~5 A sustained |

### GPIO pin map

| Function | GPIO | Notes |
|---|---|---|
| 1-Wire — fridge upper | 22 | one DS18B20 |
| 1-Wire — fridge lower | 26 | one DS18B20 |
| 1-Wire — compressor | 4 | one DS18B20 |
| 1-Wire — ambient + freezer | 33 | two DS18B20 on one bus (by address) |
| CT clamp (current) | 34 | ADC1 channel 6, input-only pin |
| Compressor relay | 5 | active-high |
| Fan relay | 18 | active-high |
| Buzzer relay (K2) | 19 | active-high, self-oscillating piezo |
| Spare relay (K1) | 21 | active-high |
| Door sensor | 39 | HIGH = open; fail-safe (reads "open" on a broken wire); input-only |
| Settings button | 35 | active-low, external pull-up; input-only |
| Panel LED 1 / 2 / 3 | 13 / 32 / 14 | active-low (driven LOW = lit) |
| Status LED (onboard) | 25 | |
| Interior light | 15 | PWM dimmed, ~1 kHz |

### Network

| Item | Detail |
|---|---|
| Wi-Fi | 2.4 GHz station, with an always-on fallback access point |
| Fallback AP | SSID "Fridgectl Fallback Hotspot" → `http://192.168.4.1/` |
| Home Assistant | MQTT with auto-discovery (appears as one "FridgeCTL" device) |
| Web interface | Port 80: `/` (read-only), `/control` (password) |
| Firmware update | Over-the-air via the web page, with automatic rollback on failure |

> Pin assignments and thresholds are defined in `src/config.h`. If you change
> wiring or tuning, that file is the single source of truth.
