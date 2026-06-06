// ================================================================
//  control.h  —  the safety-critical 5 s control loop plus the
//  derived/computed helpers used by MQTT telemetry.
// ================================================================
#pragma once

#include <string>

void control_task_start(void);   // boot sequence + 5 s control loop (WDT-subscribed)
void beeper_task_start(void);    // audible feedback on System Enable changes

// ---- Derived telemetry helpers (lock internally) ----
float       ctl_control_temp(void);
float       ctl_efficiency(void);
bool        ctl_turbo(void);
bool        ctl_blind_mode(void);
bool        ctl_high_duty(void);
bool        ctl_temp_rising(void);
bool        ctl_food_unsafe(void);
bool        ctl_initial_cooldown(void);
bool        ctl_comp_temp_fault(void);
bool        ctl_freezer_sensor_fault(void);
bool        ctl_door_warn(void);
std::string ctl_alarm_state(void);

// ---- Actions invoked from MQTT / buttons ----
void ctl_reset_overcurrent(void);
void ctl_trigger_defrost(void);
void ctl_set_turbo(bool on);
