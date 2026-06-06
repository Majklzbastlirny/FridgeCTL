// ================================================================
//  FridgeCTL — native ESP-IDF firmware
//  Migrated from ESPHome. Architecture: blocking sensor I/O lives in
//  its own FreeRTOS tasks so the safety-critical control loop (the
//  only Task-WDT subscriber) can never be starved into a watchdog
//  reset — which was the failure mode of the cooperative ESPHome loop.
// ================================================================
#include "config.h"
#include "app_state.h"
#include "nvs_store.h"
#include "gpio_io.h"
#include "sensors.h"
#include "control.h"
#include "leds.h"
#include "wifi_net.h"
#include "mqtt_net.h"
#include "ota.h"
#include "diag.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

// Publishes telemetry to MQTT and refreshes WiFi status.
static void telemetry_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(5000);
    for (;;) {
        wifi_update_status();
        mqtt_publish_all_states();
        vTaskDelay(period);
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "FridgeCTL " FW_VERSION " booting");

    // ---- Core init (order matters) ----
    state_init_mutex();
    nvs_store_init();      // nvs_flash_init + load persisted config into g
    diag_init();           // reset reason + pre-reset breadcrumb snapshot (needs NVS)
    gpio_io_init();        // relays/LEDs off, light PWM, inputs
    sensors_init();        // OneWire buses + ADC + internal temp sensor

    // ---- Networking ----
    wifi_net_start();      // STA + fallback AP
    mqtt_net_start();      // esp-mqtt + HA discovery (on connect)
    ota_start();           // HTTP OTA endpoint

    // ---- Tasks ----
    nvs_store_task_start();         // deferred config persistence (5 min)
    gpio_io_input_task_start();     // door + button
    sensors_temp_task_start();      // DS18B20 reader
    sensors_ct_task_start();        // CT clamp + inrush
    leds_task_start();              // 500 ms LED/UI
    control_task_start();           // boot sequence + 5 s control loop (WDT)
    beeper_task_start();            // audible feedback on System Enable changes

    xTaskCreate(telemetry_task, "telemetry", 6144, nullptr, 3, nullptr);

    // After an OTA, confirm or roll back the new image based on health.
    ota_health_check_start();

    ESP_LOGI(TAG, "init complete");
}
