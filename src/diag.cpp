#include "diag.h"
#include "app_state.h"

#include "esp_system.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "diag";

// ---- RTC breadcrumb ring (survives reset, not full power loss) ----
#define TRACE_LEN 12
#define TRACE_MAGIC 0xC0FFEE42

struct DiagTrace {
    uint32_t magic;
    uint32_t head;                 // next write index
    uint32_t ms[TRACE_LEN];        // millis() at the event
    uint8_t  ev[TRACE_LEN];        // DiagEvent code
};

// RTC_NOINIT: not cleared on reset, only on power-on (and even then it's
// just garbage we detect via the magic value).
static RTC_NOINIT_ATTR DiagTrace s_trace;

// Snapshot of the trace as it was BEFORE this boot wiped it (the pre-reset
// history). Filled once in diag_init().
static char s_last_trace[160];
static char s_reset_counts[64];

static const char *ev_name(uint8_t e) {
    switch (e) {
        case DIAG_BOOT:        return "BOOT";
        case DIAG_COMP_ON:     return "COMP+";
        case DIAG_COMP_OFF:    return "COMP-";
        case DIAG_FAN_ON:      return "FAN+";
        case DIAG_FAN_OFF:     return "FAN-";
        case DIAG_DEFROST_ON:  return "DEF+";
        case DIAG_DEFROST_OFF: return "DEF-";
        case DIAG_OTA:         return "OTA";
        case DIAG_SYS_ON:      return "SYS+";
        case DIAG_SYS_OFF:     return "SYS-";
        default:               return "?";
    }
}

void diag_event(uint8_t code) {
    uint32_t i = s_trace.head % TRACE_LEN;
    s_trace.ms[i] = millis();
    s_trace.ev[i] = code;
    s_trace.head++;
}

// NVS counter keys, one per reset reason of interest.
static void bump_counter(const char *key) {
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t v = 0;
    nvs_get_u32(h, key, &v);
    v++;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t get_counter(const char *key) {
    nvs_handle_t h;
    if (nvs_open("diag", NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t v = 0;
    nvs_get_u32(h, key, &v);
    nvs_close(h);
    return v;
}

void diag_init(void) {
    esp_reset_reason_t rr = esp_reset_reason();

    // 1. Snapshot the pre-reset breadcrumb trace (if valid) into a string,
    //    oldest-first, before we re-init the ring for this boot.
    if (s_trace.magic == TRACE_MAGIC && s_trace.head > 0) {
        uint32_t count = s_trace.head < TRACE_LEN ? s_trace.head : TRACE_LEN;
        uint32_t start = s_trace.head >= TRACE_LEN ? s_trace.head - TRACE_LEN : 0;
        int p = 0;
        for (uint32_t k = 0; k < count; k++) {
            uint32_t idx = (start + k) % TRACE_LEN;
            p += snprintf(s_last_trace + p, sizeof(s_last_trace) - p, "%s%s@%lus",
                          k ? " " : "", ev_name(s_trace.ev[idx]),
                          (unsigned long)(s_trace.ms[idx] / 1000));
            if (p >= (int)sizeof(s_last_trace) - 16) break;
        }
    } else {
        strlcpy(s_last_trace, "(none)", sizeof(s_last_trace));
    }

    // 2. Fresh ring for this boot.
    s_trace.magic = TRACE_MAGIC;
    s_trace.head  = 0;
    memset(s_trace.ms, 0, sizeof(s_trace.ms));
    memset(s_trace.ev, 0, sizeof(s_trace.ev));

    // 3. Bump the persistent counter for this reset reason.
    switch (rr) {
        case ESP_RST_POWERON:  bump_counter("po");   break;
        case ESP_RST_SW:       bump_counter("sw");   break;
        case ESP_RST_PANIC:    bump_counter("panic");break;
        case ESP_RST_INT_WDT:  bump_counter("intwdt");break;
        case ESP_RST_TASK_WDT: bump_counter("taskwdt");break;
        case ESP_RST_WDT:      bump_counter("wdt");  break;
        case ESP_RST_BROWNOUT: bump_counter("bo");   break;
        case ESP_RST_DEEPSLEEP:bump_counter("ds");   break;
        default:               bump_counter("unk");  break;
    }

    // 4. Build the counts string for HA.
    snprintf(s_reset_counts, sizeof(s_reset_counts),
             "PO:%lu SW:%lu PANIC:%lu INT:%lu TASK:%lu WDT:%lu BO:%lu",
             (unsigned long)get_counter("po"),
             (unsigned long)get_counter("sw"),
             (unsigned long)get_counter("panic"),
             (unsigned long)get_counter("intwdt"),
             (unsigned long)get_counter("taskwdt"),
             (unsigned long)get_counter("wdt"),
             (unsigned long)get_counter("bo"));

    ESP_LOGW(TAG, "reset reason=%d  pre-reset trace: %s", (int)rr, s_last_trace);
    ESP_LOGW(TAG, "reset counts: %s", s_reset_counts);

    diag_event(DIAG_BOOT);
}

void diag_reset_counts(char *b, size_t n) { strlcpy(b, s_reset_counts, n); }
void diag_last_trace(char *b, size_t n)   { strlcpy(b, s_last_trace, n); }
