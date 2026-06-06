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

uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
