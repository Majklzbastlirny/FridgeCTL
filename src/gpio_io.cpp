#include "gpio_io.h"
#include "config.h"
#include "app_state.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "io";

// ---- Relays ----------------------------------------------------
void relay_comp(bool on) { gpio_set_level(PIN_RELAY_COMP, on ? 1 : 0); }
void relay_fan (bool on) { gpio_set_level(PIN_RELAY_FAN,  on ? 1 : 0); }
void relay_k2  (bool on) { gpio_set_level(PIN_RELAY_K2,   on ? 1 : 0); }
void relay_k1  (bool on) { gpio_set_level(PIN_RELAY_K1,   on ? 1 : 0); }

// ---- LEDs (active-low: logical ON => drive 0) ------------------
void led1(bool on)       { gpio_set_level(PIN_LED1, on ? 0 : 1); }
void led2(bool on)       { gpio_set_level(PIN_LED2, on ? 0 : 1); }
void led3(bool on)       { gpio_set_level(PIN_LED3, on ? 0 : 1); }
void status_led(bool on) { gpio_set_level(PIN_STATUS_LED, on ? 1 : 0); }

// ---- Interior light (LEDC) -------------------------------------
#define LIGHT_TIMER   LEDC_TIMER_0
#define LIGHT_CHANNEL LEDC_CHANNEL_0
#define LIGHT_MODE    LEDC_LOW_SPEED_MODE

void light_set(bool on, float brightness) {
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;
    uint32_t maxv = (1u << LIGHT_PWM_RES_BITS) - 1;
    uint32_t duty = on ? (uint32_t)(brightness * maxv + 0.5f) : 0;
    // Hardware fade: ISR-driven ramp to the target duty over LIGHT_FADE_MS.
    // Non-blocking (LEDC_FADE_NO_WAIT) so no task is ever stalled by the ramp.
    ledc_set_fade_with_time(LIGHT_MODE, LIGHT_CHANNEL, duty, LIGHT_FADE_MS);
    ledc_fade_start(LIGHT_MODE, LIGHT_CHANNEL, LEDC_FADE_NO_WAIT);
}

// ---- Init ------------------------------------------------------
void gpio_io_init(void) {
    // Outputs: relays, LEDs, status LED
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pin_bit_mask =
        (1ULL << PIN_RELAY_COMP) | (1ULL << PIN_RELAY_FAN) |
        (1ULL << PIN_RELAY_K2)   | (1ULL << PIN_RELAY_K1)  |
        (1ULL << PIN_LED1) | (1ULL << PIN_LED2) | (1ULL << PIN_LED3) |
        (1ULL << PIN_STATUS_LED);
    gpio_config(&out);

    // Fail-safe: everything off at boot
    relay_comp(false); relay_fan(false); relay_k2(false); relay_k1(false);
    led1(false); led2(false); led3(false); status_led(false);

    // Door input (HIGH = open). GPIO39 is input-only, no internal pulls.
    gpio_config_t din = {};
    din.mode = GPIO_MODE_INPUT;
    din.pin_bit_mask = (1ULL << PIN_DOOR);
    din.pull_up_en = GPIO_PULLUP_DISABLE;
    din.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&din);

    // Button input (active-low, external 4k7 pull-up). GPIO35 input-only.
    gpio_config_t btn = {};
    btn.mode = GPIO_MODE_INPUT;
    btn.pin_bit_mask = (1ULL << PIN_BUTTON);
    btn.pull_up_en = GPIO_PULLUP_DISABLE;
    btn.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&btn);

    // LEDC for interior light PWM
    ledc_timer_config_t lt = {};
    lt.speed_mode      = LIGHT_MODE;
    lt.timer_num       = LIGHT_TIMER;
    lt.duty_resolution = (ledc_timer_bit_t)LIGHT_PWM_RES_BITS;
    lt.freq_hz         = LIGHT_PWM_FREQ_HZ;
    lt.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&lt);

    ledc_channel_config_t lc = {};
    lc.gpio_num   = PIN_LIGHT_PWM;
    lc.speed_mode = LIGHT_MODE;
    lc.channel    = LIGHT_CHANNEL;
    lc.timer_sel  = LIGHT_TIMER;
    lc.duty       = 0;
    lc.hpoint     = 0;
    ledc_channel_config(&lc);

    // Install the LEDC fade service so light_set() can use hardware fades.
    ledc_fade_func_install(0);

    ESP_LOGI(TAG, "GPIO initialised, all outputs off");
}

// ---- Door + button input task ----------------------------------
//  Polled at 10 ms with 50 ms debounce. Button classified into
//  short / double / long press to match the ESPHome on_multi_click.

#define POLL_MS         10
#define DEBOUNCE_TICKS  (INPUT_DEBOUNCE_MS / POLL_MS)
#define DOUBLE_GAP_MS   500    // max gap between presses for a double
#define LONG_PRESS_MS   3000   // hold >= this -> long press

static void input_task(void *arg) {
    // Door debounce
    bool door_stable = false;
    bool door_last_raw = false;
    int  door_count = 0;

    // Button debounce (pressed = level LOW)
    bool btn_stable = false;
    bool btn_last_raw = false;
    int  btn_count = 0;

    // Button classifier
    uint32_t press_start = 0;
    bool     awaiting_second = false;   // a short press is pending double check
    uint32_t first_release = 0;
    bool     long_fired = false;

    // System on/off combo: button held + door opened 3x within COMBO_WINDOW_MS.
    int      combo_opens = 0;           // door-open count during this hold
    uint32_t combo_first = 0;           // time of first open in the window
    bool     door_during_hold = false;  // any door open seen this hold (suppresses turbo)

    for (;;) {
        uint32_t now = millis();

        // ---- Door ----
        bool draw = gpio_get_level(PIN_DOOR) != 0;   // HIGH = open
        if (draw == door_last_raw) {
            if (door_count < DEBOUNCE_TICKS) door_count++;
        } else {
            door_count = 0;
            door_last_raw = draw;
        }
        if (door_count >= DEBOUNCE_TICKS && draw != door_stable) {
            door_stable = draw;
            on_door_change(door_stable);

            // Combo detection: count door-OPEN edges while button is held.
            if (door_stable && btn_stable) {
                door_during_hold = true;          // this hold is a combo, not turbo
                if (combo_opens == 0 || (now - combo_first) > COMBO_WINDOW_MS) {
                    combo_opens = 1;
                    combo_first = now;
                } else {
                    combo_opens++;
                }
                if (combo_opens >= COMBO_DOOR_OPENS) {
                    combo_opens = 0;
                    on_combo_toggle_system();
                }
            }
        }

        // ---- Button (active-low) ----
        bool braw = gpio_get_level(PIN_BUTTON) == 0;  // pressed = LOW
        if (braw == btn_last_raw) {
            if (btn_count < DEBOUNCE_TICKS) btn_count++;
        } else {
            btn_count = 0;
            btn_last_raw = braw;
        }
        if (btn_count >= DEBOUNCE_TICKS && braw != btn_stable) {
            btn_stable = braw;
            if (btn_stable) {
                // press edge
                press_start = now;
                long_fired = false;
                combo_opens = 0;
                door_during_hold = false;
                on_button_press_raw();
            } else {
                // release edge
                combo_opens = 0;
                uint32_t dur = now - press_start;
                if (dur >= LONG_PRESS_MS) {
                    // already handled below while held; ignore
                } else if (awaiting_second &&
                           (now - first_release) <= DOUBLE_GAP_MS) {
                    awaiting_second = false;
                    on_button_double();
                } else {
                    awaiting_second = true;
                    first_release = now;
                }
            }
        }

        // Long press fires while still held — but only as TURBO if the door
        // wasn't used during this hold (door + hold is reserved for the combo).
        if (btn_stable && !long_fired && (now - press_start) >= LONG_PRESS_MS) {
            long_fired = true;
            awaiting_second = false;
            if (!door_during_hold) on_button_long();
        }

        // Pending single (short) press times out -> fire short
        if (awaiting_second && (now - first_release) > DOUBLE_GAP_MS) {
            awaiting_second = false;
            on_button_short();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void gpio_io_input_task_start(void) {
    xTaskCreate(input_task, "input", 4096, nullptr, 6, nullptr);
}
