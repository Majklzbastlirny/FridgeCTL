#include "leds.h"
#include "config.h"
#include "app_state.h"
#include "gpio_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

// Ambient-aware high-duty threshold (mirrors control.cpp).
static float high_duty_threshold(float amb) {
    float thr = 0.85f;
    if (!isnan(amb)) {
        thr = 0.55f + amb * 0.01f;
        if (thr < 0.60f) thr = 0.60f;
        if (thr > 0.95f) thr = 0.95f;
    }
    return thr;
}

// Interior light auto-management (door driven), separate from panel LEDs.
static void update_interior_light(uint32_t now) {
    StateGuard lock;
    if (g.door_phys) {
        // Dim to 25% when system OFF (visual off-state cue), full when on.
        float want_bri = g.system_enable ? 1.0f : 0.25f;
        if (!g.light_on || g.light_brightness != want_bri) {
            g.light_on = true;
            g.light_brightness = want_bri;
            light_set(true, want_bri);
        }
    } else if (g.light_off_at != 0 && (int32_t)(now - g.light_off_at) >= 0) {
        g.light_off_at = 0;
        if (g.light_on) {
            g.light_on = false;
            light_set(false, g.light_brightness);
        }
    }
}

// Onboard status LED: solid = MQTT up, slow blink = WiFi only, fast = down.
static void update_status_led(uint32_t tick) {
    StateGuard lock;
    if (g.mqtt_connected) {
        status_led(true);
    } else if (g.wifi_connected) {
        status_led((tick % 4) < 2);
    } else {
        status_led((tick % 2) == 0);
    }
}

static void led_tick(void) {
    uint32_t now = millis();
    update_interior_light(now);

    StateGuard lock;
    if (!g.boot_done) return;

    g.led_tick++;
    uint8_t tick = g.led_tick;
    update_status_led(tick);

    bool slow_blink = (tick % 4) < 2;   // 2 s period
    bool fast_blink = (tick % 2) == 0;  // 1 s period

    // ---- System disabled: LED1 slow pulse, rest off ----
    if (!g.system_enable) {
        led1(slow_blink);
        led2(false);
        led3(false);
        return;
    }

    // ---- Setpoint display (3 s after change) ----
    if ((int32_t)(g.setpoint_show_until - now) > 0) {
        float sp = g.target_temp;
        int level;
        if (sp <= 2.5f)      level = 1;
        else if (sp <= 4.5f) level = 2;
        else if (sp <= 6.5f) level = 3;
        else                 level = 4;

        if (level == 4) {
            led1(fast_blink); led2(fast_blink); led3(fast_blink);
        } else {
            led1(level >= 1);
            led2(level >= 2);
            led3(level >= 3);
        }
        return;
    }

    // ---- Normal status display ----
    bool u_ok = valid_chamber(g.temp_upper);
    bool l_ok = valid_chamber(g.temp_lower);
    bool sensor_ok = u_ok || l_ok;
    bool turbo = (int32_t)(g.turbo_until_ms - now) > 0;
    bool blind = !u_ok && !l_ok;

    // LED1: system — solid OK, fast blink degraded/fault
    if (sensor_ok && !g.overcurrent_fault) led1(true);
    else                                    led1(fast_blink);

    // LED2: cooling — solid running, fast blink turbo, slow blink defrost, off idle
    if (turbo)               led2(fast_blink);
    else if (g.defrost_active) led2(slow_blink);
    else if (g.comp_on)      led2(true);
    else                     led2(false);

    // LED3: alarm — solid critical, slow blink warning, off OK
    bool critical = g.overcurrent_fault || g.comp_overtemp || blind;
    if (g.food_unsafe_ms > 0 && (uint32_t)(now - g.food_unsafe_ms) > FOOD_UNSAFE_MS)
        critical = true;

    bool warning = false;
    if (!u_ok && l_ok) warning = true;
    if (u_ok && !l_ok) warning = true;
    float tc = g.temp_compressor;
    if (isnan(tc) || tc < TEMP_COMP_MIN_C || tc > TEMP_COMP_MAX_C) warning = true;
    if (g.no_current_count >= CT_NO_CURRENT_COUNT) warning = true;
    if (g.intermittent_zeros >= CAP_FAULT_COUNT) warning = true;
    if (g.comp_on && g.temp_history > -900.0f) {
        float tv = g.temp_control;
        if (!isnan(tv) && tv > g.temp_history + 0.5f) warning = true;
    }
    if (g.duty_pct > high_duty_threshold(g.temp_ambient)) warning = true;
    bool door_eff = g.door_phys && !g.door_override;
    if (door_eff && g.door_open_ms > 0 &&
        (uint32_t)(now - g.door_open_ms) > DOOR_WARN_MS) warning = true;
    if (g.comp_blocked) warning = true;

    if (critical)      led3(true);
    else if (warning)  led3(slow_blink);
    else               led3(false);
}

static void leds_task(void *arg) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        led_tick();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(LED_PERIOD_MS));
    }
}

void leds_task_start(void) {
    xTaskCreate(leds_task, "leds", 4096, nullptr, 5, nullptr);
}
