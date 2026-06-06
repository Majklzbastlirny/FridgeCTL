#include "control.h"
#include "config.h"
#include "app_state.h"
#include "gpio_io.h"
#include "nvs_store.h"
#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "ctrl";

// Ambient-aware high-duty threshold (0.60 .. 0.95).
static float high_duty_threshold(float amb) {
    float thr = 0.85f;
    if (!isnan(amb)) {
        thr = 0.55f + amb * 0.01f;
        if (thr < 0.60f) thr = 0.60f;
        if (thr > 0.95f) thr = 0.95f;
    }
    return thr;
}

// ================================================================
//  Main control tick — faithful port of the ESPHome 5 s interval.
//  Runs entirely under the state lock; performs no blocking I/O.
// ================================================================
static void control_tick(void) {
    StateGuard lock;
    uint32_t now = millis();
    if (!g.boot_done) return;

    // ---- OTA in progress: do NOT touch relays. ----
    // The OTA handler manages relays itself (compressor off up front, fan kept
    // on through the flash write, fan off only after). If the control loop
    // toggled a relay here it could fire the fan's inductive transient during
    // the cache-off flash write — the exact pileup that crashes the chip. We
    // still bump the heartbeat so the rollback health check stays satisfied.
    if (g.ota_in_progress) { g.control_beats++; return; }

    // ---- System disabled: everything off ----
    if (!g.system_enable) {
        if (g.comp_on) { relay_comp(false); g.comp_on = false; g.comp_stop_ms = now; }
        if (g.fan_on)  { relay_fan(false);  g.fan_on = false; }
        if (g.k2_on)   { relay_k2(false);   g.k2_on = false; }
        // Re-arm initial-cooldown: while off the chamber warms, so on the
        // next enable we again report INITIAL COOLDOWN until it pulls down.
        g.reached_setpoint = false;
        g.control_beats++;     // keep the OTA heartbeat alive even when off
        return;
    }

    // ---- Read sensors ----
    float t_upper = g.temp_upper;
    float t_lower = g.temp_lower;
    float t_comp  = g.temp_compressor;
    float current = g.compressor_current;
    float setpoint = g.target_temp;
    if (isnan(setpoint)) setpoint = DEFAULT_SETPOINT_C;
    if (g.vacation_mode) {
        float vt = g.vacation_temp;
        if (!isnan(vt)) setpoint = vt;
    }
    bool comp_on = g.comp_on;

    bool door_phys = g.door_phys;
    bool door_open = door_phys && !g.door_override;

    // ---- Sensor sanity ----
    bool upper_ok = valid_chamber(t_upper);
    bool lower_ok = valid_chamber(t_lower);
    bool comp_temp_ok = !isnan(t_comp) && t_comp > TEMP_COMP_MIN_C && t_comp < TEMP_COMP_MAX_C;

    // ---- Control temperature ----
    float t_ctrl = NAN;
    bool blind_mode = false;
    if (upper_ok && lower_ok) t_ctrl = (t_upper + t_lower) / 2.0f;
    else if (upper_ok)        t_ctrl = t_upper;
    else if (lower_ok)        t_ctrl = t_lower;
    else                      blind_mode = true;

    // ---- Initial-cooldown latch ----
    // After a cold start (board powered up with the chamber near room temp),
    // the system isn't "ready" until it has pulled the temperature down into
    // the control band at least once. Latches true the first time t_ctrl
    // reaches the band; surfaced as the "INITIAL COOLDOWN" alarm state until
    // then. Blind mode (no chamber sensor) can't judge this, so we treat it
    // as reached to avoid masking the BLIND alarm.
    if (!g.reached_setpoint) {
        if (blind_mode || (!isnan(t_ctrl) && t_ctrl <= setpoint + HYST_ABOVE_C)) {
            g.reached_setpoint = true;
            if (!blind_mode)
                ESP_LOGI(TAG, "Initial cooldown complete (t=%.1f sp=%.1f)", t_ctrl, setpoint);
        }
    }

    // ---- Safety: compressor overtemp (hysteresis) ----
    if (comp_temp_ok) {
        if (t_comp > COMP_OVERTEMP_ON_C && !g.comp_overtemp) {
            g.comp_overtemp = true;
            ESP_LOGW(TAG, "Compressor OVERTEMP (%.1f C)", t_comp);
        } else if (t_comp < COMP_OVERTEMP_OFF_C && g.comp_overtemp) {
            g.comp_overtemp = false;
            ESP_LOGI(TAG, "Compressor temp OK (%.1f C)", t_comp);
        }
    }

    // ---- Safety: overcurrent (3 consecutive ticks = 15 s) ----
    if (!isnan(current) && current > OVERCURRENT_A && comp_on) {
        if (g.overcurrent_count < 255) g.overcurrent_count++;
        if (g.overcurrent_count >= OVERCURRENT_COUNT && !g.overcurrent_fault) {
            g.overcurrent_fault = true;
            ESP_LOGW(TAG, "OVERCURRENT FAULT (%.2f A)", current);
        }
    } else {
        g.overcurrent_count = 0;
    }

    // ---- CT / capacitor fault ----
    if (comp_on && (uint32_t)(now - g.comp_start_ms) > COMP_RUNNING_GRACE_MS) {
        if (!isnan(current) && current < 0.1f) {
            if (g.no_current_count < 255) g.no_current_count++;
            if (g.no_current_count == CT_NO_CURRENT_COUNT)
                ESP_LOGW(TAG, "CT FAULT - no current while compressor ON");
            if (g.intermittent_zeros < 255) g.intermittent_zeros++;
            if (g.intermittent_zeros == CAP_FAULT_COUNT)
                ESP_LOGW(TAG, "POSSIBLE CAP FAULT - overload cycling");
        } else {
            g.no_current_count = 0;
        }
    } else if (!comp_on) {
        g.no_current_count = 0;
    }

    // ---- Food safety: track time above 8 C ----
    if (!isnan(t_ctrl)) {
        if (t_ctrl > FOOD_UNSAFE_C) {
            if (g.food_unsafe_ms == 0) {
                g.food_unsafe_ms = now;
                ESP_LOGW(TAG, "Food safety: temp above 8 C (%.1f)", t_ctrl);
            }
        } else {
            if (g.food_unsafe_ms > 0) ESP_LOGI(TAG, "Food safety: temp back below 8 C");
            g.food_unsafe_ms = 0;
        }
    }

    // ---- Freezer temperature monitoring (informational) ----
    float t_frz = g.temp_freezer;
    if (!isnan(t_frz) && t_frz > TEMP_FREEZER_MIN_C && t_frz < TEMP_FREEZER_MAX_C) {
        float warm_thr = g.freezer_warm_thr;
        float cold_thr = g.freezer_cold_thr;
        float crit_thr = g.freezer_crit_thr;
        if (isnan(warm_thr)) warm_thr = -12.0f;
        if (isnan(cold_thr)) cold_thr = -28.0f;
        if (isnan(crit_thr)) crit_thr = 0.0f;

        // Too warm
        if (t_frz > warm_thr) {
            if (g.freezer_warm_ms == 0) {
                g.freezer_warm_ms = now;
                ESP_LOGW(TAG, "Freezer above %.1f C (%.1f) - arming", warm_thr, t_frz);
            }
            if (!g.freezer_warm_latched &&
                (uint32_t)(now - g.freezer_warm_ms) > FREEZER_LATCH_MS) {
                g.freezer_warm_latched = true;
                ESP_LOGW(TAG, "Freezer WARM alarm LATCHED");
            }
        } else if (t_frz < warm_thr - FREEZER_HYST_C) {
            g.freezer_warm_ms = 0;
            if (g.freezer_warm_latched) {
                g.freezer_warm_latched = false;
                ESP_LOGI(TAG, "Freezer warm alarm cleared (hysteresis)");
            }
        }
        // Too cold
        if (t_frz < cold_thr) {
            if (g.freezer_cold_ms == 0) {
                g.freezer_cold_ms = now;
                ESP_LOGW(TAG, "Freezer below %.1f C (%.1f) - arming", cold_thr, t_frz);
            }
            if (!g.freezer_cold_latched &&
                (uint32_t)(now - g.freezer_cold_ms) > FREEZER_LATCH_MS) {
                g.freezer_cold_latched = true;
                ESP_LOGW(TAG, "Freezer COLD alarm LATCHED");
            }
        } else if (t_frz > cold_thr + FREEZER_HYST_C) {
            g.freezer_cold_ms = 0;
            if (g.freezer_cold_latched) {
                g.freezer_cold_latched = false;
                ESP_LOGI(TAG, "Freezer cold alarm cleared (hysteresis)");
            }
        }
        // Critical (food safety): 2 h sustained
        if (t_frz > crit_thr) {
            if (g.freezer_unsafe_ms == 0) {
                g.freezer_unsafe_ms = now;
                ESP_LOGW(TAG, "Freezer FOOD SAFETY arming: above %.1f C (%.1f)", crit_thr, t_frz);
            }
            if (!g.freezer_unsafe_latched &&
                (uint32_t)(now - g.freezer_unsafe_ms) > FREEZER_UNSAFE_MS) {
                g.freezer_unsafe_latched = true;
                ESP_LOGW(TAG, "Freezer FOOD SAFETY LATCHED");
            }
        } else {
            g.freezer_unsafe_ms = 0;
            if (g.freezer_unsafe_latched) {
                g.freezer_unsafe_latched = false;
                ESP_LOGI(TAG, "Freezer food safety cleared");
            }
        }
    }

    // ---- Defrost management ----
    if (g.defrost_active) {
        uint32_t dur_ms = (uint32_t)(g.defrost_duration_min) * 60000;
        if (dur_ms < 300000) dur_ms = 300000;   // floor 5 min
        if ((uint32_t)(now - g.defrost_start_ms) >= dur_ms) {
            g.defrost_active = false;
            diag_event(DIAG_DEFROST_OFF);
            ESP_LOGI(TAG, "DEFROST complete");
        }
    }

    // ---- Door open duration ----
    uint32_t door_dur = 0;
    if (door_open && g.door_open_ms > 0) door_dur = (uint32_t)(now - g.door_open_ms);

    // ---- Turbo active? ----
    bool turbo = (int32_t)(g.turbo_until_ms - now) > 0;

    // ---- Determine cooling demand ----
    bool want = false;
    if (g.defrost_active) {
        want = false;
    } else if (blind_mode) {
        uint32_t on_ms  = (uint32_t)(g.duty_on_min)  * 60000;
        uint32_t off_ms = (uint32_t)(g.duty_off_min) * 60000;
        if (on_ms < 300000)  on_ms  = 300000;
        if (off_ms < 300000) off_ms = 300000;
        if (g.duty_phase_ms == 0) {
            g.duty_phase_ms = now;
            g.duty_on = true;
            ESP_LOGW(TAG, "BLIND DUTY CYCLE active");
        }
        uint32_t phase = (uint32_t)(now - g.duty_phase_ms);
        if (g.duty_on && phase >= on_ms) {
            g.duty_on = false; g.duty_phase_ms = now;
        } else if (!g.duty_on && phase >= off_ms) {
            g.duty_on = true;  g.duty_phase_ms = now;
        }
        want = g.duty_on;
        if (turbo) want = true;
    } else {
        g.duty_phase_ms = 0;
        if (turbo) {
            want = true;
        } else if (t_ctrl > setpoint + HYST_ABOVE_C) {
            want = true;
        } else if (t_ctrl < setpoint - HYST_BELOW_C) {
            want = false;
        } else {
            want = comp_on;   // dead-band: maintain
        }
    }

    // ---- Safety overrides (always win) ----
    if (g.overcurrent_fault)        want = false;
    if (g.comp_overtemp)            want = false;
    if (door_dur > DOOR_COMP_CUTOFF_MS) want = false;

    // ---- Minimum off time (3 min) ----
    g.comp_blocked = false;
    if (want && !comp_on && g.comp_stop_ms != 0) {
        if ((uint32_t)(now - g.comp_stop_ms) < COMP_MIN_OFF_MS) {
            want = false;
            g.comp_blocked = true;
        }
    }

    // ---- Apply compressor state ----
    if (want && !comp_on) {
        relay_comp(true);
        diag_event(DIAG_COMP_ON);
        g.comp_on = true;
        g.comp_start_ms = now;
        g.comp_starts++;
        g.intermittent_zeros = 0;
        g.inrush_peak = 0.0f;                // reset for fresh inrush capture
        g.eff_start_temp = isnan(t_ctrl) ? -999.0f : t_ctrl;
        ESP_LOGI(TAG, "Compressor ON  (t=%.1f sp=%.1f %s%s)",
                 isnan(t_ctrl) ? -999.0f : t_ctrl, setpoint,
                 turbo ? "TURBO " : "", blind_mode ? "BLIND" : "");
    } else if (!want && comp_on) {
        relay_comp(false);
        diag_event(DIAG_COMP_OFF);
        g.comp_on = false;
        g.comp_stop_ms = now;
        if (g.eff_start_temp > -900.0f && !isnan(t_ctrl)) {
            float run_min = (float)((uint32_t)(now - g.comp_start_ms)) / 60000.0f;
            if (run_min > 5.0f) g.eff_value = (g.eff_start_temp - t_ctrl) / run_min;
        }
        ESP_LOGI(TAG, "Compressor OFF (t=%.1f sp=%.1f eff=%.3f%s)",
                 isnan(t_ctrl) ? -999.0f : t_ctrl, setpoint,
                 g.eff_value, blind_mode ? " BLIND" : "");
    }

    // ---- Fan logic ----
    bool want_fan = false;
    if (g.defrost_active) {
        want_fan = true;
    } else if (g.comp_on) {
        want_fan = true;
        g.fan_runon_until = now + FAN_RUNON_MS;
    } else if ((int32_t)(g.fan_runon_until - now) > 0) {
        want_fan = true;
    }
    if (door_dur > DOOR_FAN_CUTOFF_MS && !g.defrost_active) want_fan = false;

    if (want_fan && !g.fan_on) { relay_fan(true);  g.fan_on = true;  diag_event(DIAG_FAN_ON); }
    if (!want_fan && g.fan_on) { relay_fan(false); g.fan_on = false; diag_event(DIAG_FAN_OFF); }

    // ---- Buzzer (K2 piezo) ----
    bool should_buzz = false;
    bool silenced = (int32_t)(g.buzzer_silence_until - now) > 0;
    if (!silenced) {
        if (g.overcurrent_fault) should_buzz = true;
        if (g.comp_overtemp)     should_buzz = true;
        if (blind_mode)          should_buzz = true;
        if (g.food_unsafe_ms > 0 && (uint32_t)(now - g.food_unsafe_ms) > FOOD_UNSAFE_MS)
            should_buzz = true;
        if (door_dur > DOOR_WARN_MS && ((now / 5000) % 2 == 0))
            should_buzz = true;
    }
    if (should_buzz && !g.k2_on) { relay_k2(true);  g.k2_on = true; }
    if (!should_buzz && g.k2_on) { relay_k2(false); g.k2_on = false; }

    // ---- Runtime + defrost counters ----
    if (g.comp_on) {
        g.runtime_tick++;
        g.defrost_tick++;
        if (g.defrost_tick >= 12) {          // every 60 s of compressor time
            g.defrost_tick = 0;
            g.defrost_comp_min++;
        }
        uint16_t df_thresh = (uint16_t)(g.defrost_interval_hours) * 60;
        if (df_thresh < 60) df_thresh = 60;
        if (g.defrost_comp_min >= df_thresh && !g.defrost_active) {
            g.defrost_active = true;
            diag_event(DIAG_DEFROST_ON);
            g.defrost_start_ms = now;
            g.defrost_comp_min = 0;
            ESP_LOGI(TAG, "DEFROST started (%d min)", (int)g.defrost_duration_min);
        }
        if (g.runtime_tick >= 120) {         // every 10 min
            g.runtime_tick = 0;
            g.comp_runtime_min += 10;
        }
    } else {
        if (g.runtime_tick >= 12) {
            g.comp_runtime_min += (g.runtime_tick * 5 + 30) / 60;
        }
        g.runtime_tick = 0;
        g.defrost_tick = 0;
    }

    // ---- Duty cycle EMA (~1 h time constant) ----
    g.duty_pct = g.duty_pct * (1.0f - DUTY_EMA_ALPHA) + (g.comp_on ? DUTY_EMA_ALPHA : 0.0f);

    // ---- Temperature history (10 min window) ----
    g.temp_hist_tick++;
    if (g.temp_hist_tick >= TEMP_HIST_TICKS) {
        g.temp_hist_tick = 0;
        if (!isnan(t_ctrl)) g.temp_history = t_ctrl;
    }

    // ---- Update computed control temperature snapshot ----
    g.temp_control = t_ctrl;

    g.control_beats++;     // liveness heartbeat (OTA rollback health check)
    nvs_store_mark_dirty();
}

// ================================================================
//  Boot sequence (LED self-test + warmup) then control loop.
// ================================================================
static void boot_sequence(void) {
    led1(true); led2(true); led3(true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    led1(false); vTaskDelay(pdMS_TO_TICKS(200));
    led2(false); vTaskDelay(pdMS_TO_TICKS(200));
    led3(false);
    vTaskDelay(pdMS_TO_TICKS(BOOT_WARMUP_MS));
    {
        StateGuard lock;
        g.comp_stop_ms = millis();   // enforce min-off after power loss
        g.boot_done = true;
    }
    ESP_LOGI(TAG, "Boot complete, control loop active");
}

static void control_task(void *arg) {
    // Subscribe THIS task to the task watchdog. It never blocks on I/O,
    // so a stuck sensor can no longer take the safety loop down.
    esp_task_wdt_add(NULL);

    boot_sequence();

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        control_tick();
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void control_task_start(void) {
    // High priority, but it yields via vTaskDelayUntil every 5 s.
    xTaskCreate(control_task, "control", 6144, nullptr, 10, nullptr);
}

// ================================================================
//  Beeper task — central audible feedback for System Enable changes.
//
//  The piezo is self-oscillating on relay K2, so pulsing the relay makes
//  it sound. This task watches system_enable for an edge and plays a
//  pattern, regardless of WHERE the toggle came from (physical combo,
//  Home Assistant, or the web page) — one place, consistent feedback.
//
//  K2 is shared with the control loop's alarm buzzer. We only pulse it
//  briefly here, then leave g.k2_on reflecting "off"; the 5 s control
//  tick re-asserts the correct alarm state immediately afterward. A real
//  alarm is at most delayed by one tick, which is inaudible in practice.
// ================================================================
static void beep(int on_ms, int off_ms) {
    relay_k2(true);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    relay_k2(false);
    { StateGuard lock; g.k2_on = false; }
    if (off_ms) vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void beeper_task(void *arg) {
    bool last_sys, last_otafail;
    { StateGuard lock; last_sys = g.system_enable; last_otafail = g.ota_failed; }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool sys, otafail, ready;
        { StateGuard lock; sys = g.system_enable; otafail = g.ota_failed; ready = g.boot_done; }
        if (!ready) continue;

        // OTA failure (rising edge): distinct error pattern, 3 long chirps.
        if (otafail && !last_otafail) {
            for (int i = 0; i < 3; i++) beep(300, 150);
        }
        last_otafail = otafail;

        // System enable change: ON = two short chirps, OFF = one long chirp.
        if (sys != last_sys) {
            last_sys = sys;
            diag_event(sys ? DIAG_SYS_ON : DIAG_SYS_OFF);
            if (sys) { beep(120, 120); beep(120, 0); }
            else     { beep(450, 0); }
        }
    }
}

void beeper_task_start(void) {
    xTaskCreate(beeper_task, "beeper", 3072, nullptr, 4, nullptr);
}

// ================================================================
//  Derived telemetry helpers
// ================================================================
float ctl_control_temp(void) {
    StateGuard lock;
    bool u = valid_chamber(g.temp_upper);
    bool l = valid_chamber(g.temp_lower);
    if (u && l) return (g.temp_upper + g.temp_lower) / 2.0f;
    if (u) return g.temp_upper;
    if (l) return g.temp_lower;
    return NAN;
}

float ctl_efficiency(void) {
    StateGuard lock;
    if (g.comp_on && g.eff_start_temp > -900.0f) {
        float run_min = (float)(millis() - g.comp_start_ms) / 60000.0f;
        if (run_min > 1.0f) {
            float t = g.temp_control;
            if (!isnan(t)) return (g.eff_start_temp - t) / run_min;
        }
    }
    return g.eff_value;
}

bool ctl_turbo(void) {
    StateGuard lock;
    return (int32_t)(g.turbo_until_ms - millis()) > 0;
}

bool ctl_blind_mode(void) {
    StateGuard lock;
    if (!g.boot_done) return false;
    return !valid_chamber(g.temp_upper) && !valid_chamber(g.temp_lower);
}

bool ctl_high_duty(void) {
    StateGuard lock;
    return g.duty_pct > high_duty_threshold(g.temp_ambient);
}

bool ctl_temp_rising(void) {
    StateGuard lock;
    if (!g.comp_on) return false;
    if (g.temp_history < -900.0f) return false;
    float t = g.temp_control;
    if (isnan(t)) return false;
    return t > g.temp_history + 0.5f;
}

bool ctl_food_unsafe(void) {
    StateGuard lock;
    if (g.food_unsafe_ms == 0) return false;
    return (uint32_t)(millis() - g.food_unsafe_ms) > FOOD_UNSAFE_MS;
}

bool ctl_initial_cooldown(void) {
    StateGuard lock;
    if (!g.boot_done || !g.system_enable) return false;
    return !g.reached_setpoint;
}

bool ctl_comp_temp_fault(void) {
    StateGuard lock;
    if (!g.boot_done) return false;
    float t = g.temp_compressor;
    return isnan(t) || t < TEMP_COMP_MIN_C || t > TEMP_COMP_MAX_C;
}

bool ctl_freezer_sensor_fault(void) {
    StateGuard lock;
    if (!g.boot_done) return false;
    float t = g.temp_freezer;
    return isnan(t) || t < TEMP_FREEZER_MIN_C || t > TEMP_FREEZER_MAX_C;
}

bool ctl_door_warn(void) {
    StateGuard lock;
    bool door_eff = g.door_phys && !g.door_override;
    if (door_eff && g.door_open_ms > 0)
        return (uint32_t)(millis() - g.door_open_ms) > DOOR_WARN_MS;
    return false;
}

std::string ctl_alarm_state(void) {
    StateGuard lock;
    uint32_t now = millis();
    if (!g.boot_done) return "BOOTING";
    if (g.ota_in_progress) return "FIRMWARE UPDATE";
    if (!g.system_enable) return "SYSTEM OFF";
    if (g.overcurrent_fault) return "OVERCURRENT FAULT";
    if (g.comp_overtemp)     return "COMPRESSOR OVERTEMP";

    bool u_ok = valid_chamber(g.temp_upper);
    bool l_ok = valid_chamber(g.temp_lower);
    if (!u_ok && !l_ok) return "BLIND DUTY CYCLE";
    if (!u_ok)          return "UPPER SENSOR FAULT";
    if (!l_ok)          return "LOWER SENSOR FAULT";

    float tc = g.temp_compressor;
    bool tc_ok = !isnan(tc) && tc > TEMP_COMP_MIN_C && tc < TEMP_COMP_MAX_C;
    bool ct_ok = g.no_current_count < CT_NO_CURRENT_COUNT;

    bool door_eff = g.door_phys && !g.door_override;
    if (door_eff && g.door_open_ms > 0 &&
        (uint32_t)(now - g.door_open_ms) > DOOR_WARN_MS)
        return "DOOR OPEN";

    if (g.food_unsafe_ms > 0 && (uint32_t)(now - g.food_unsafe_ms) > FOOD_UNSAFE_MS)
        return "FOOD SAFETY ALERT";
    if (g.freezer_unsafe_latched) return "FREEZER FOOD SAFETY";
    if (g.defrost_active) return "DEFROST";
    if (g.comp_blocked)   return "COMPRESSOR COOLDOWN";

    if (!tc_ok) return "COMP TEMP SENSOR FAULT";
    if (!ct_ok) return "CT SENSOR FAULT";
    if (g.intermittent_zeros >= CAP_FAULT_COUNT) return "POSSIBLE CAP FAULT";
    if (g.freezer_warm_latched) return "FREEZER TOO WARM";
    if (g.freezer_cold_latched) return "FREEZER TOO COLD";

    // Last firmware upload aborted (latched until a successful OTA or reboot).
    if (g.ota_failed) return "FIRMWARE UPDATE FAILED";

    // System still pulling down from a cold start — not fully ready yet.
    if (!g.reached_setpoint) return "INITIAL COOLDOWN";

    if (g.comp_on && g.temp_history > -900.0f) {
        float t = g.temp_control;
        if (!isnan(t) && t > g.temp_history + 0.5f) return "TEMP RISING WHILE COOLING";
    }
    if (g.duty_pct > high_duty_threshold(g.temp_ambient)) return "HIGH DUTY CYCLE";
    return "OK";
}

// ================================================================
//  Actions (MQTT / buttons)
// ================================================================
void ctl_reset_overcurrent(void) {
    StateGuard lock;
    g.overcurrent_fault = false;
    g.overcurrent_count = 0;
    ESP_LOGI(TAG, "Overcurrent fault RESET");
}

void ctl_trigger_defrost(void) {
    StateGuard lock;
    g.defrost_active = true;
    g.defrost_start_ms = millis();
    g.defrost_comp_min = 0;
    ESP_LOGI(TAG, "Manual defrost triggered");
}

void ctl_set_turbo(bool on) {
    StateGuard lock;
    if (on) {
        g.turbo_until_ms = millis() + TURBO_DURATION_MS;
        ESP_LOGI(TAG, "Turbo ON (30 min)");
    } else {
        g.turbo_until_ms = 0;
        ESP_LOGI(TAG, "Turbo OFF");
    }
}

// ================================================================
//  Button + door handlers (called from the input task)
// ================================================================
void on_button_press_raw(void) {
    StateGuard lock;
    if (g.k2_on) {
        g.buzzer_silence_until = millis() + BUZZER_SILENCE_MS;
        relay_k2(false);
        g.k2_on = false;
        ESP_LOGI(TAG, "Buzzer silenced 10 min");
    }
}

void on_button_short(void) {
    // Cycle target 2 -> 4 -> 6 -> 8 -> 2
    StateGuard lock;
    float sp = g.target_temp;
    if (sp < 3.0f)      sp = 4.0f;
    else if (sp < 5.0f) sp = 6.0f;
    else if (sp < 7.0f) sp = 8.0f;
    else                sp = 2.0f;
    g.target_temp = sp;
    g.setpoint_show_until = millis() + SETPOINT_SHOW_MS;
    nvs_store_mark_dirty();
    ESP_LOGI(TAG, "Setpoint -> %.1f C", sp);
}

void on_button_double(void) {
    bool now_on;
    {
        StateGuard lock;
        g.door_override = !g.door_override;
        now_on = g.door_override;
        nvs_store_mark_dirty();
    }
    ESP_LOGI(TAG, "Door override %s (via button)", now_on ? "ON" : "OFF");
    // LED feedback (lock released): 2 blinks = ON, 1 blink = OFF.
    led3(true); vTaskDelay(pdMS_TO_TICKS(200)); led3(false);
    if (now_on) {
        vTaskDelay(pdMS_TO_TICKS(200));
        led3(true); vTaskDelay(pdMS_TO_TICKS(200)); led3(false);
    }
}

void on_button_long(void) {
    bool now_on;
    {
        StateGuard lock;
        uint32_t now = millis();
        if ((int32_t)(g.turbo_until_ms - now) > 0) {
            g.turbo_until_ms = 0;
            now_on = false;
            ESP_LOGI(TAG, "Turbo CANCELLED");
        } else {
            g.turbo_until_ms = now + TURBO_DURATION_MS;
            now_on = true;
            ESP_LOGI(TAG, "Turbo ON (30 min)");
        }
    }
    (void)now_on;
    // Triple-flash confirmation
    for (int i = 0; i < 3; i++) {
        led1(true); led2(true); led3(true);
        vTaskDelay(pdMS_TO_TICKS(150));
        led1(false); led2(false); led3(false);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

// Button held + door cycled 3x: toggle the whole system on/off. Designed as
// a deliberate two-input gesture so a single stuck button or flapping door
// cannot trigger it. Same gesture toggles both directions.
void on_combo_toggle_system(void) {
    bool now_on;
    {
        StateGuard lock;
        g.system_enable = !g.system_enable;
        now_on = g.system_enable;
        nvs_store_mark_dirty();
    }
    ESP_LOGW(TAG, "SYSTEM %s (via door+button combo)", now_on ? "ENABLED" : "DISABLED");

    // Audible feedback is handled centrally by the beeper task (so HA / web
    // toggles chirp too). Here we add a quick LED triple-flash for local
    // confirmation at the panel.
    for (int i = 0; i < 3; i++) {
        led1(true); led2(true); led3(true);
        vTaskDelay(pdMS_TO_TICKS(120));
        led1(false); led2(false); led3(false);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
}

void on_door_change(bool open) {
    StateGuard lock;
    g.door_phys = open;
    if (open) {
        g.door_open_ms = millis();
        g.light_off_at = 0;
        g.light_on = true;
        // Dim to 25% when the system is OFF so an opened door signals the
        // off state at a glance; full brightness during normal operation.
        g.light_brightness = g.system_enable ? 1.0f : 0.25f;
        light_set(true, g.light_brightness);
    } else {
        g.door_open_ms = 0;
        g.light_off_at = millis() + 3000;   // auto-off 3 s after close
    }
}
