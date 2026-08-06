// ================================================================
//  sensors.h  —  DS18B20 temperature task + CT-clamp current task.
//  Both are low/medium priority and never block the control loop.
// ================================================================
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void sensors_init(void);            // ADC + OneWire bus init
void sensors_temp_task_start(void); // DS18B20 reader
void sensors_ct_task_start(void);   // CT clamp RMS + inrush capture
float sensors_internal_temp(void);  // ESP32 die temperature (NAN if unavailable)

// ---- 1-Wire bus scan -------------------------------------------
// Enumerates every DS18B20 on every bus so the web UI can show what is
// physically there and let the user say which ROM is the freezer and which
// is the ambient probe. The scan is blocking bit-banged I/O, so it always
// runs inside temp_task - callers only raise a request flag.

#define OW_BUS_COUNT 4                 // upper, lower, compressor, shared
#define OW_SCAN_MAX  (OW_BUS_COUNT * 4)

struct OwFound {
    uint8_t  bus;    // index into the bus table (sensors_bus_info)
    uint64_t rom;    // 64-bit ROM code, same encoding as ADDR_* in config.h
    float    temp;   // raw reading at scan time, NO calibration offset
};

struct OwBusInfo {
    const char *name;        // human label for the UI
    uint8_t     gpio;
    const char *fixed_role;  // role hard-wired to this bus, "" for the shared one
};
// Bus metadata, single source of truth for the UI. nullptr if out of range.
const OwBusInfo *sensors_bus_info(int bus);

void sensors_request_scan(void);       // ask temp_task to rescan (returns at once)
bool sensors_scan_pending(void);       // a scan is queued or running
bool sensors_scan_done(void);          // at least one scan has completed
// Copy the last scan results out under the state lock. Returns the count;
// *age_ms (if non-null) gets the ms since that scan finished.
int  sensors_scan_results(OwFound *out, int max, uint32_t *age_ms);
// Tell the temp task the ambient/freezer ROM assignment changed, so it drops
// the fault debounce rather than holding a value from the *other* probe.
void sensors_roles_changed(void);
