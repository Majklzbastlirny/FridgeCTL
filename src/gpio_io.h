// ================================================================
//  gpio_io.h  —  relays, panel LEDs, interior light PWM, and the
//  door + settings-button input task (debounce + multi-click).
// ================================================================
#pragma once

#include <stdbool.h>

void gpio_io_init(void);
void gpio_io_input_task_start(void);

// ---- Relays (active-high) ----
void relay_comp(bool on);
void relay_fan(bool on);
void relay_k2(bool on);     // buzzer
void relay_k1(bool on);     // spare

// ---- Panel LEDs (active-low driven, but pass logical on/off) ----
void led1(bool on);
void led2(bool on);
void led3(bool on);
void status_led(bool on);

// ---- Interior light (LEDC PWM) ----
void light_set(bool on, float brightness);   // brightness 0..1

// ---- Button/door event handlers (implemented in control.cpp) ----
//  Declared here so the input task can dispatch them.
void on_button_press_raw(void);   // any press edge (used to silence buzzer)
void on_button_short(void);
void on_button_double(void);
void on_button_long(void);
void on_combo_toggle_system(void);   // button-held + door-cycled-3x: toggle System Enable
void on_door_change(bool open);
