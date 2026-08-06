// ================================================================
//  logbuf.h  —  in-RAM ring of the most recent log lines.
//
//  Hooks esp_log's vprintf so every ESP_LOGx line is captured and can
//  be read back over HTTP (/api/logs) without a USB cable. Serial
//  output is untouched: the hook chains to the previous handler.
//
//  The hook runs on whatever task logged, so it must never block —
//  it formats on the caller's stack and takes only a short spinlock
//  around the copy into the ring. It NEVER touches the state mutex.
// ================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// Install the vprintf hook. Call as the very first thing in app_main so the
// boot banner and the reset-reason diagnostics are captured too. Safe before
// state_init_mutex() — this module owns its own lock.
void logbuf_init(void);

// Sequence number of the newest line stored (0 = nothing logged yet). Lines
// are numbered from 1 and never reused, so a client can poll for "anything
// newer than N" without re-fetching what it already has.
uint32_t logbuf_seq(void);

// Copy the lines with sequence > `since` into buf as newline-terminated text,
// oldest first. Returns bytes written (never exceeds n-1, always NUL
// terminated). *out_seq, if non-null, receives the sequence of the last line
// copied — pass it back as `since` on the next call.
//
// If `since` is older than the ring, the copy simply starts at the oldest
// line still held; the caller sees a gap, not stale data.
int logbuf_dump(char *buf, size_t n, uint32_t since, uint32_t *out_seq);
