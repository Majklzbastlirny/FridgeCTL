// ================================================================
//  ow.h  —  compact bit-banged 1-Wire + DS18B20 driver.
//  Runs only inside the (non-critical) sensor task. Each timing slot
//  is wrapped in a tiny critical section so interrupts are masked for
//  ~70 us at a time, never long enough to disturb WiFi/the WDT.
//
//  Buses rely on the external 4.7k pull-up that is already on the
//  board: we only ever drive the line LOW (output) or release it
//  (input) — never drive it HIGH.
// ================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

// ---- low level 1-Wire ----
void    ow_init(gpio_num_t pin);
bool    ow_reset(gpio_num_t pin);          // true = device presence detected
void    ow_write_byte(gpio_num_t pin, uint8_t b);
uint8_t ow_read_byte(gpio_num_t pin);
uint8_t ow_crc8(const uint8_t *data, int len);

// ---- DS18B20 helpers ----
// Trigger a conversion on every device on the bus (SKIP ROM + 0x44).
// Caller must wait >= 750 ms before reading.
bool ds18b20_convert_all(gpio_num_t pin);

// Read the single device on a bus (SKIP ROM). Returns NAN on failure.
float ds18b20_read_single(gpio_num_t pin);

// Read a specific device addressed by its 64-bit ROM code (MATCH ROM).
// Used for the two sensors that share the ambient bus.
float ds18b20_read_addr(gpio_num_t pin, uint64_t rom);
