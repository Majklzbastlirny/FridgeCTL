// ================================================================
//  app_state.h  —  all shared runtime state in one struct, guarded
//  by a single recursive mutex. Mirrors the ESPHome globals plus
//  live sensor readings and HA-settable numbers/switches.
//
//  Access pattern: take state_lock() for the duration of a coherent
//  read-modify-write, release with state_unlock(). The control task
//  is the main writer of control-domain fields; the sensor task
//  writes the sensor fields; MQTT writes setpoints/switch requests.
// ================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

struct AppState {
    // ---- Boot ----
    bool     boot_done       = false;
    uint32_t control_beats   = 0;       // ++ every control tick; liveness proof
                                        // (used by OTA rollback health check)

    // ---- OTA status (not persisted) ----
    bool     ota_in_progress = false;   // upload underway -> "FIRMWARE UPDATE"
    bool     ota_failed      = false;   // last upload aborted -> latched alarm

    // ---- Live sensor readings (NAN = invalid/unavailable) ----
    float    temp_upper      = NAN;
    float    temp_lower      = NAN;
    float    temp_compressor = NAN;
    float    temp_ambient    = NAN;
    float    temp_freezer    = NAN;
    float    compressor_current = NAN;
    float    adc_raw         = NAN;     // diagnostic CT Vrms

    // ---- Computed ----
    float    temp_control    = NAN;
    bool     reached_setpoint = false;  // latched once temp first enters band
                                        // (not persisted; "INITIAL COOLDOWN")
    float    duty_pct        = 0.0f;    // persist  (0..1 EMA)
    float    eff_value       = 0.0f;    // persist  (C/min, last cycle)
    float    eff_start_temp  = -999.0f;
    float    inrush_peak     = 0.0f;

    // ---- Compressor stats ----
    uint32_t comp_runtime_min = 0;      // persist
    uint32_t comp_starts      = 0;      // persist
    uint8_t  runtime_tick     = 0;

    // ---- Timing (millis-style, ms since boot) ----
    uint32_t comp_stop_ms     = 0;
    uint32_t comp_start_ms    = 0;
    uint32_t turbo_until_ms   = 0;
    uint32_t door_open_ms     = 0;
    uint32_t fan_runon_until  = 0;
    uint32_t setpoint_show_until = 0;
    uint32_t buzzer_silence_until = 0;
    uint32_t duty_phase_ms    = 0;
    uint32_t defrost_start_ms = 0;
    uint32_t food_unsafe_ms   = 0;
    uint32_t freezer_warm_ms  = 0;
    uint32_t freezer_cold_ms  = 0;
    uint32_t freezer_unsafe_ms = 0;

    // ---- Safety latches / flags ----
    bool     comp_overtemp    = false;
    bool     overcurrent_fault = false;
    uint8_t  overcurrent_count = 0;
    bool     comp_blocked     = false;   // min-off-time block this cycle
    uint8_t  no_current_count = 0;
    uint8_t  intermittent_zeros = 0;

    // ---- Blind duty cycle ----
    bool     duty_on          = true;

    // ---- Defrost ----
    uint16_t defrost_comp_min = 0;       // persist
    uint8_t  defrost_tick     = 0;
    bool     defrost_active   = false;

    // ---- Freezer latched alarms (persist) ----
    bool     freezer_warm_latched   = false;
    bool     freezer_cold_latched   = false;
    bool     freezer_unsafe_latched = false;

    // ---- Rate-of-change history ----
    float    temp_history     = -999.0f; // persist
    uint8_t  temp_hist_tick   = 0;

    // ---- LED tick ----
    uint8_t  led_tick         = 0;

    // ---- Relay / output logical states ----
    bool     comp_on          = false;
    bool     fan_on           = false;
    bool     k2_on            = false;   // buzzer
    bool     k1_on            = false;   // spare relay
    bool     light_on         = false;
    float    light_brightness = 1.0f;    // 0..1
    uint32_t light_off_at     = 0;       // scheduled auto-off (door closed +3s)

    // ---- Physical inputs ----
    bool     door_phys        = false;   // raw door sensor (HIGH = open)

    // ---- HA-settable numbers (persist) ----
    float    target_temp      = 4.0f;
    float    duty_on_min      = 30.0f;
    float    duty_off_min     = 30.0f;
    float    defrost_interval_hours = 8.0f;
    float    defrost_duration_min   = 20.0f;
    float    vacation_temp    = 7.0f;
    float    freezer_warm_thr = -12.0f;
    float    freezer_cold_thr = -28.0f;
    float    freezer_crit_thr = 0.0f;

    // ---- Per-sensor temperature calibration offsets (°C, persist) ----
    // Added to each raw DS18B20 reading before publishing. Default 0 = no
    // correction. Estimated from a stabilised all-at-ambient soak (no truth
    // sensor, so offsets are relative to the group mean). See MANUAL.
    float    off_upper        = 0.0f;
    float    off_lower        = 0.0f;
    float    off_compressor   = 0.0f;
    float    off_ambient      = 0.0f;
    float    off_freezer      = 0.0f;

    // ---- HA-settable switches (persist where noted) ----
    bool     system_enable    = true;    // persist (default ON)
    bool     door_override    = false;   // persist (default OFF)
    bool     vacation_mode    = false;   // persist (default OFF)
    bool     freezer_sensor_enable = true; // persist (default ON); OFF = ignore
                                           // freezer probe (no fault, no alarms)

    // ---- Net status (for text/diag sensors) ----
    bool     wifi_connected   = false;
    bool     mqtt_connected   = false;
    int8_t   wifi_rssi        = 0;
    char     ip_addr[16]      = "0.0.0.0";
    char     ssid[33]         = "";
    char     mac_addr[18]     = "";
};

extern AppState g;

// Recursive mutex so nested lock() inside helpers is safe.
void state_init_mutex(void);
void state_lock(void);
void state_unlock(void);

// Convenience RAII-ish guard for C++ scopes.
struct StateGuard {
    StateGuard()  { state_lock(); }
    ~StateGuard() { state_unlock(); }
};

// millis() equivalent — ms since boot, wraps at ~49 days like Arduino.
uint32_t millis(void);

// Sensor sanity helpers.
static inline bool valid_chamber(float t) {
    return !isnan(t) && t > -40.0f && t < 50.0f;
}
