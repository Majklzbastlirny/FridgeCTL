// ================================================================
//  diag.h  —  black-box diagnostics for reset investigation.
//
//  Two mechanisms:
//   1. A breadcrumb ring buffer in RTC memory (RTC_NOINIT) that
//      survives software/watchdog/brownout resets (but not a true
//      power-on). After a reset we snapshot what was there so you can
//      see the last events leading up to it — e.g. confirm a reset
//      always follows a FAN- (fan relay opening, inductive kickback).
//   2. Persistent per-reason reset counters in NVS (long-term tally).
// ================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

// Breadcrumb event codes.
enum DiagEvent {
    DIAG_BOOT = 1,
    DIAG_COMP_ON,
    DIAG_COMP_OFF,
    DIAG_FAN_ON,
    DIAG_FAN_OFF,
    DIAG_DEFROST_ON,
    DIAG_DEFROST_OFF,
    DIAG_OTA,
    DIAG_SYS_ON,
    DIAG_SYS_OFF,
};

void diag_init(void);            // read reset reason, snapshot RTC trace, bump NVS counters
void diag_event(uint8_t code);   // record a breadcrumb into RTC memory

// HA text-sensor getters.
void diag_reset_counts(char *b, size_t n);   // e.g. "PO:3 WDT:2 BO:1"
void diag_last_trace(char *b, size_t n);      // pre-reset breadcrumbs, chronological
