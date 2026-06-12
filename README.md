# FridgeCTL — native ESP-IDF firmware

Smart fridge controller for the **LilyGo T-Relay (ESP32)**, migrated from
ESPHome to native ESP-IDF (PlatformIO). Same behaviour and hardware map as
the original `fridgectl.yaml`, but with a watchdog-proof task architecture
and Home Assistant integration over **MQTT Discovery**.

## Why the rewrite

The ESPHome build ran everything in one cooperative loop. The four
bit-banged DS18B20 buses, the 200 ms CT-clamp sample and WiFi/API
reconnects all shared that loop; a flaky sensor stalled it and the
**task watchdog** rebooted the board ("timeouts → watchdog kills
everything").

This firmware splits the work across FreeRTOS tasks:

| Task         | Prio | Job                                                  |
|--------------|------|------------------------------------------------------|
| `control`    | 10   | 5 s safety/control state machine. **Only WDT subscriber.** Never blocks on I/O. |
| `input`      | 6    | Door + button (short/double/long press) debounce     |
| `leds`       | 5    | 500 ms panel-LED / status-LED / interior-light UI    |
| `temp`       | 4    | DS18B20 reads (uses `vTaskDelay` over the 750 ms conversion) |
| `ct`         | 4    | CT-clamp RMS + inrush capture                        |
| `telemetry`  | 3    | Publishes MQTT state every 5 s                        |
| `nvs_commit` | 2    | Flushes persisted config every 5 min                 |

A stuck OneWire read now only delays the sensor task; the control loop
keeps feeding the watchdog and keeps the compressor safe.

## Layout

```
platformio.ini        build config (env:fridgectl)
partitions.csv        2× OTA app slots (4 MB flash)
sdkconfig.defaults    WDT timeouts, flash size, etc.
CMakeLists.txt        IDF project root
src/
  secrets.h           YOUR WiFi/MQTT/OTA credentials (git-ignored)
  secrets.h.example   template
  config.h            pins, thresholds, CT calibration  ← tune here
  app_state.*         shared state struct + mutex
  nvs_store.*         persistence
  ow.*                bit-banged 1-Wire + DS18B20 driver
  sensors.*           DS18B20 tasks + CT/ADC RMS
  gpio_io.*           relays / LEDs / light PWM / door+button task
  control.*           the 5 s control loop + derived telemetry
  leds.*              500 ms LED/UI loop
  wifi_net.*          STA + always-on fallback AP
  mqtt_net.*          esp-mqtt + HA Discovery + command routing
  ota.*               HTTP firmware-upload endpoint
  main.cpp            init + task creation
```

## First build & flash

1. Copy credentials template and fill it in (already done if `src/secrets.h`
   exists):
   ```
   cp src/secrets.h.example src/secrets.h
   ```
2. Build + flash over USB:
   ```
   pio run -t upload
   pio device monitor
   ```
   The first build downloads the ESP-IDF toolchain — give it a while.

## OTA updates

After the first USB flash the device serves an upload page:

- Browse to `http://<device-ip>/` (Basic-Auth: `OTA_USERNAME` / `OTA_PASSWORD`)
  and upload `.pio/build/fridgectl/firmware.bin`, **or**
- ```
  curl -u admin:PASS --data-binary @.pio/build/fridgectl/firmware.bin http://<device-ip>/update
  ```

Relays are forced off before flashing; the board reboots into the new image
on success. Two OTA slots mean a bad upload won't brick the device.

## Home Assistant

On MQTT connect the firmware publishes retained **MQTT Discovery** configs,
so every entity from the original config (sensors, binary sensors, numbers,
switches, buttons, the interior light and the Alarm State text) auto-appears
under one `FridgeCTL` device. Availability uses an LWT on
`fridgectl/status`.

- State topics:   `fridgectl/<object>/state`
- Command topics: `fridgectl/<object>/cmd`

## Tuning / calibration (`src/config.h`)

- **CT clamp**: `CT_CAL_SLOPE` / `CT_CAL_OFFSET` are the least-squares fit of
  the original ESPHome `calibrate_linear` points. `CT_NOISE_FLOOR_A` (0.30 A)
  zeroes idle noise.
- **DS18B20 addresses**: the ambient bus (GPIO33) carries two sensors,
  matched by ROM code `ADDR_AMBIENT` / `ADDR_FREEZER`. If one reads as
  unavailable, check the boot log and update these.
- **Per-sensor temperature offsets**: each probe has a runtime-settable trim
  (−5…+5 °C, default 0) exposed as a *Temp Offset* number in Home Assistant and
  the web control page — no rebuild needed. Added to the raw reading before it's
  displayed/alarmed/used for control, and persisted in NVS. See MANUAL.md for
  how to estimate the offsets from an all-at-ambient soak.
- All control thresholds (setpoint hysteresis, min-off time, overtemp,
  overcurrent, freezer/defrost/food-safety timers) are `#define`s near the
  bottom of `config.h`, named to match the ESPHome originals.

## Pin map (unchanged from ESPHome)

| Function            | GPIO | Notes                         |
|---------------------|------|-------------------------------|
| OneWire upper       | 22   | 1 DS18B20                     |
| OneWire lower       | 26   | 1 DS18B20                     |
| OneWire compressor  | 4    | 1 DS18B20                     |
| OneWire ambient     | 33   | ambient + freezer (by addr)   |
| CT clamp ADC        | 34   | ADC1_CH6                      |
| Compressor relay    | 5    |                               |
| Fan relay           | 18   |                               |
| Buzzer (K2)         | 19   |                               |
| K1 spare relay      | 21   |                               |
| Door sensor         | 39   | HIGH = open (fail-safe open)  |
| Settings button     | 35   | active-low, ext. 4k7 pull-up  |
| Panel LED 1/2/3     | 13/32/14 | active-low sink            |
| Status LED          | 25   |                               |
| Interior light PWM  | 15   | LEDC 1 kHz                    |

## Sensor mounting

The stainless-shelled DS18B20 probes can hang in **air** or — better for the two
fridge-chamber probes — sit in a small sealed vial of **propylene glycol**
(~30–50 % in distilled water) as a thermal buffer that mimics food temperature,
smoothing control and suppressing door-open spikes. Keep the **compressor** probe
clamped to the compressor body (fast over-temp response) and the **ambient** probe
in open air. Defrost timing is driven by compressor run-time, not sensor
temperature, so buffering does not affect it. See the User Manual for details.

## License

FridgeCTL is free software, licensed under the **GNU General Public License,
version 3** — see [`LICENSE`](LICENSE).

You may use, study, modify and redistribute it. If you distribute it or any
modified version (including inside a product you sell), you must make the
complete corresponding **source available under the same GPLv3 terms**, and —
for consumer hardware — provide the information needed to install modified
firmware. There is **no warranty**.

```
Copyright (C) 2026 Michal Basler

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 3, as published by the
Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.
```
