#include "logbuf.h"

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// 120 x 160 B ~= 19 KB of static DRAM. Static on purpose: it must not eat
// into the heap headroom that WiFi and the OTA image buffer need.
#define LOG_LINES     120
#define LOG_LINE_LEN  160

static char     s_lines[LOG_LINES][LOG_LINE_LEN];
static uint32_t s_head = 0;      // lines ever written; newest line's sequence
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// Partial line being assembled. One log line is not always one vprintf call —
// the WiFi blob emits its header and its body as two separate calls, which on
// the serial console land on one line. We only commit a ring entry on '\n', so
// the ring reproduces exactly what the serial monitor shows.
static char   s_pend[LOG_LINE_LEN];
static size_t s_pend_len = 0;

// Both callers already hold s_mux.
static void commit_pending(void) {
    if (s_pend_len == 0) return;
    s_pend[s_pend_len] = 0;
    memcpy(s_lines[s_head % LOG_LINES], s_pend, s_pend_len + 1);
    s_head++;
    s_pend_len = 0;
}

// The handler that was installed before us (normally the plain UART vprintf).
// Chained on every line so `pio device monitor` behaves exactly as before.
static vprintf_like_t s_prev = nullptr;

// Called on whatever task emitted the log line, and IDF puts the log path in
// IRAM so it can also come from ISR context. Rules: no allocation, no
// blocking, and never the state mutex (state_lock() waits forever and would
// invert priority against the WDT-subscribed control task). The formatting
// happens on the caller's stack, OUTSIDE the lock; the spinlock only covers
// the copy into the ring.
static int log_vprintf(const char *fmt, va_list ap) {
    char line[LOG_LINE_LEN];

    va_list cp;
    va_copy(cp, ap);                 // ap is still needed by the real handler
    int n = vsnprintf(line, sizeof(line), fmt, cp);
    va_end(cp);

    if (n > 0) {
        int len = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
        portENTER_CRITICAL_SAFE(&s_mux);
        for (int i = 0; i < len; i++) {
            char c = line[i];
            if (c == '\r') continue;
            if (c == '\n') { commit_pending(); continue; }
            if (s_pend_len >= sizeof(s_pend) - 1) commit_pending();  // over-long
            s_pend[s_pend_len++] = c;
        }
        portEXIT_CRITICAL_SAFE(&s_mux);
    }

    return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void logbuf_init(void) {
    s_prev = esp_log_set_vprintf(log_vprintf);
}

uint32_t logbuf_seq(void) {
    portENTER_CRITICAL_SAFE(&s_mux);
    uint32_t h = s_head;
    portEXIT_CRITICAL_SAFE(&s_mux);
    return h;
}

int logbuf_dump(char *buf, size_t n, uint32_t since, uint32_t *out_seq) {
    if (!buf || n == 0) return 0;
    buf[0] = 0;
    size_t p = 0;

    portENTER_CRITICAL_SAFE(&s_mux);
    uint32_t head = s_head;
    portEXIT_CRITICAL_SAFE(&s_mux);

    // Sequence of the newest line that has already been overwritten. Anything
    // at or below it is gone, so a client that fell behind resumes from the
    // oldest line still held rather than getting nothing.
    uint32_t oldest = head > LOG_LINES ? head - LOG_LINES : 0;
    uint32_t last   = since > oldest ? since : oldest;

    for (uint32_t seq = last + 1; seq <= head; seq++) {
        char line[LOG_LINE_LEN];
        bool live;
        portENTER_CRITICAL_SAFE(&s_mux);
        live = (uint32_t)(s_head - seq) < LOG_LINES;   // not recycled under us
        if (live) memcpy(line, s_lines[(seq - 1) % LOG_LINES], LOG_LINE_LEN);
        portEXIT_CRITICAL_SAFE(&s_mux);
        if (!live) { last = seq; continue; }
        line[LOG_LINE_LEN - 1] = 0;

        size_t len = strlen(line);
        if (p + len + 2 > n) break;      // no room for line + '\n' + NUL
        memcpy(buf + p, line, len);
        p += len;
        buf[p++] = '\n';
        last = seq;                      // caller resumes here next poll
    }

    buf[p] = 0;
    if (out_seq) *out_seq = last;
    return (int)p;
}
