// ================================================================
//  sensors.h  —  DS18B20 temperature task + CT-clamp current task.
//  Both are low/medium priority and never block the control loop.
// ================================================================
#pragma once

void sensors_init(void);            // ADC + OneWire bus init
void sensors_temp_task_start(void); // DS18B20 reader
void sensors_ct_task_start(void);   // CT clamp RMS + inrush capture
float sensors_internal_temp(void);  // ESP32 die temperature (NAN if unavailable)
