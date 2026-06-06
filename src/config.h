// ================================================================
//  config.h  —  pins, timing, thresholds, calibration
//  Board: LilyGo T-Relay (ESP32)  —  matches the original ESPHome map
// ================================================================
#pragma once

#include "driver/gpio.h"
#include "hal/adc_types.h"

// ----------------------------------------------------------------
//  Device identity (used for MQTT topics + HA discovery)
// ----------------------------------------------------------------
#define DEVICE_NAME          "FridgeCTL"
#define DEVICE_ID            "fridgectl"          // MQTT base + HA object_id prefix
#define DISCOVERY_PREFIX     "homeassistant"      // HA default
#define FW_VERSION           "2.0.0-idf"

// ----------------------------------------------------------------
//  OneWire buses (one DS18B20 per bus, except the ambient bus which
//  carries TWO sensors addressed by ROM code).
// ----------------------------------------------------------------
#define PIN_OW_UPPER         GPIO_NUM_22
#define PIN_OW_LOWER         GPIO_NUM_26
#define PIN_OW_COMPRESSOR    GPIO_NUM_4
#define PIN_OW_AMBIENT       GPIO_NUM_33   // ambient + freezer share this bus

// DS18B20 ROM addresses on the shared ambient bus (from the boot log).
// Family code 0x28 is in the low byte; this matches the IDF onewire_bus
// 64-bit address encoding and the values printed by ESPHome.
// If a sensor isn't found, check the boot log for the actual address.
#define ADDR_AMBIENT         0xbd0623c34c9e4a28ULL
#define ADDR_FREEZER         0x4E00001096F82428ULL

// ----------------------------------------------------------------
//  CT clamp (compressor current) — ADC1
// ----------------------------------------------------------------
#define PIN_ADC_CT_CHANNEL   ADC_CHANNEL_6        // GPIO34 = ADC1_CH6
#define ADC_CT_ATTEN         ADC_ATTEN_DB_12      // ~0..3.3V
#define CT_SAMPLE_MS         200                  // RMS window (matches ESPHome)

// calibrate_linear least-squares fit of the original 3 points:
//   0 -> 0 , 0.04303 -> 1.17 , 0.2912 -> 7.94   (x = ADC Vrms, y = Amps)
//   y = CT_CAL_SLOPE * x + CT_CAL_OFFSET
#define CT_CAL_SLOPE         27.2712f
#define CT_CAL_OFFSET        -0.00157f
#define CT_NOISE_FLOOR_A     0.30f                // below this -> 0 A

// ----------------------------------------------------------------
//  Relays (active-high)
// ----------------------------------------------------------------
#define PIN_RELAY_K1         GPIO_NUM_21   // K1 spare (HA-exposed)
#define PIN_RELAY_K2         GPIO_NUM_19   // buzzer / piezo
#define PIN_RELAY_FAN        GPIO_NUM_18   // evaporator fan
#define PIN_RELAY_COMP       GPIO_NUM_5    // compressor

// ----------------------------------------------------------------
//  Inputs
// ----------------------------------------------------------------
#define PIN_DOOR             GPIO_NUM_39   // HIGH = open (input-only, ext. wiring)
#define PIN_BUTTON           GPIO_NUM_35   // active-low, external 4k7 pullup (input-only)
#define INPUT_DEBOUNCE_MS    50

// System on/off combo: hold button + open the door this many times within
// the window. Requires both inputs active in a pattern that never occurs
// in normal use, so a single stuck/failed input can't trigger it.
#define COMBO_DOOR_OPENS     3
#define COMBO_WINDOW_MS      5000

// ----------------------------------------------------------------
//  Panel LEDs (active-low sink drivers) + interior light PWM
// ----------------------------------------------------------------
#define PIN_LED1             GPIO_NUM_13   // active-low
#define PIN_LED2             GPIO_NUM_32   // active-low
#define PIN_LED3             GPIO_NUM_14   // active-low
#define PIN_STATUS_LED       GPIO_NUM_25   // onboard status LED

#define PIN_LIGHT_PWM        GPIO_NUM_15   // interior light (LEDC)
#define LIGHT_PWM_FREQ_HZ    1000
#define LIGHT_PWM_RES_BITS   10            // 0..1023
#define LIGHT_FADE_MS        1000          // interior light fade in/out time

// ----------------------------------------------------------------
//  Control loop tuning (all ported 1:1 from the ESPHome config)
// ----------------------------------------------------------------
#define CONTROL_PERIOD_MS        5000      // main control loop
#define LED_PERIOD_MS            500       // LED/UI loop
#define BOOT_WARMUP_MS           30000     // sensor warmup before control active

#define COMP_OVERTEMP_ON_C       75.0f     // compressor overtemp latch
#define COMP_OVERTEMP_OFF_C      55.0f     // clear (hysteresis)
#define COMP_MIN_OFF_MS          180000    // 3 min minimum off time
#define COMP_INRUSH_WINDOW_MS    3000      // latch inrush peak in first 3 s

#define OVERCURRENT_A            5.0f      // overcurrent threshold
#define OVERCURRENT_COUNT        3         // consecutive 5 s ticks -> fault

#define HYST_ABOVE_C             1.5f      // turn on when t > sp + this
#define HYST_BELOW_C             0.5f      // turn off when t < sp - this

#define DEFAULT_SETPOINT_C       4.0f
#define FAN_RUNON_MS             120000    // 2 min fan run-on after compressor
#define DOOR_FAN_CUTOFF_MS       60000     // door open > 1 min -> fan off
#define DOOR_COMP_CUTOFF_MS      300000    // door open > 5 min -> compressor off
#define DOOR_WARN_MS             120000    // door open > 2 min -> warning/buzzer

#define FOOD_UNSAFE_C            8.0f      // above this starts food-safety timer
#define FOOD_UNSAFE_MS           7200000   // 2 h sustained -> alert

#define FREEZER_LATCH_MS         1800000   // 30 min sustained -> warm/cold latch
#define FREEZER_UNSAFE_MS        7200000   // 2 h above critical -> food safety
#define FREEZER_HYST_C           1.0f

#define TURBO_DURATION_MS        1800000   // 30 min
#define BUZZER_SILENCE_MS        600000    // 10 min
#define SETPOINT_SHOW_MS         3000      // LED setpoint feedback window

#define DUTY_EMA_ALPHA           (1.0f / 720.0f)   // ~1 h time constant @ 5 s
#define TEMP_HIST_TICKS          120       // 120 * 5 s = 10 min rate window

// Sensor sanity bounds (DS18B20: 85C = power-on reset, out-of-range = dead)
#define TEMP_CHAMBER_MIN_C       -40.0f
#define TEMP_CHAMBER_MAX_C       50.0f
#define TEMP_COMP_MIN_C          -40.0f
#define TEMP_COMP_MAX_C          120.0f
#define TEMP_FREEZER_MIN_C       -50.0f
#define TEMP_FREEZER_MAX_C       50.0f

// ----------------------------------------------------------------
//  Sensor update intervals (ms)
// ----------------------------------------------------------------
#define DS18B20_PERIOD_MS        30000     // chamber/compressor reads
#define DS18B20_AMBIENT_DIV      2         // ambient/freezer every 2nd cycle (~60s)

// ----------------------------------------------------------------
//  Fault-detection counters
// ----------------------------------------------------------------
#define CT_NO_CURRENT_COUNT      6         // zero current while ON -> CT fault
#define CAP_FAULT_COUNT          5         // intermittent zeros -> cap fault
#define COMP_RUNNING_GRACE_MS    60000     // ignore CT faults in first 60 s
