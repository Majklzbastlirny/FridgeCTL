#include "app_state.h"
#include "esp_timer.h"

AppState g;

static SemaphoreHandle_t s_mutex = nullptr;

void state_init_mutex(void) {
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
        configASSERT(s_mutex);
    }
}

void state_lock(void) {
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void state_unlock(void) {
    xSemaphoreGiveRecursive(s_mutex);
}

// Test hook: build with -DFRIDGECTL_CLOCK_OFFSET_MS=2147183648 (2^31 - 5 min)
// to boot just short of the 24.8-day signed-wrap point and watch every timer
// cross it, or 4294667296 (2^32 - 5 min) for the 49.7-day unsigned wrap.
// Never ship an image built with it.
#ifndef FRIDGECTL_CLOCK_OFFSET_MS
#define FRIDGECTL_CLOCK_OFFSET_MS 0
#endif

uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL) + (uint32_t)FRIDGECTL_CLOCK_OFFSET_MS;
}
