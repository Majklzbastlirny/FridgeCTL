#include "nvs_store.h"
#include "app_state.h"
#include "config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "nvs";
static const char *NS  = "fridge";
static volatile bool s_dirty = false;

// Helpers --------------------------------------------------------
static void get_u32(nvs_handle_t h, const char *k, uint32_t *v) {
    nvs_get_u32(h, k, v);
}
static void get_u16(nvs_handle_t h, const char *k, uint16_t *v) {
    nvs_get_u16(h, k, v);
}
static void get_u8(nvs_handle_t h, const char *k, uint8_t *v) {
    nvs_get_u8(h, k, v);
}
static void get_bool(nvs_handle_t h, const char *k, bool *v) {
    uint8_t t = *v ? 1 : 0;
    if (nvs_get_u8(h, k, &t) == ESP_OK) *v = (t != 0);
}
static void get_f(nvs_handle_t h, const char *k, float *v) {
    size_t sz = sizeof(float);
    nvs_get_blob(h, k, v, &sz);
}
static void set_f(nvs_handle_t h, const char *k, float v) {
    nvs_set_blob(h, k, &v, sizeof(float));
}

void nvs_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    nvs_store_load();
}

void nvs_store_load(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no stored config yet, using defaults");
        return;
    }
    StateGuard lock;
    // Stats
    get_u32(h, "runtime_min", &g.comp_runtime_min);
    get_u32(h, "starts",      &g.comp_starts);
    get_u16(h, "df_comp_min", &g.defrost_comp_min);
    get_f(h,   "duty_pct",    &g.duty_pct);
    get_f(h,   "eff_value",   &g.eff_value);
    get_f(h,   "temp_hist",   &g.temp_history);
    // Freezer latches
    get_bool(h, "frz_warm_l",  &g.freezer_warm_latched);
    get_bool(h, "frz_cold_l",  &g.freezer_cold_latched);
    get_bool(h, "frz_unsafe_l",&g.freezer_unsafe_latched);
    // Numbers
    get_f(h, "target",      &g.target_temp);
    get_f(h, "duty_on",     &g.duty_on_min);
    get_f(h, "duty_off",    &g.duty_off_min);
    get_f(h, "df_interval", &g.defrost_interval_hours);
    get_f(h, "df_duration", &g.defrost_duration_min);
    get_f(h, "vac_temp",    &g.vacation_temp);
    get_f(h, "frz_warm",    &g.freezer_warm_thr);
    get_f(h, "frz_cold",    &g.freezer_cold_thr);
    get_f(h, "frz_crit",    &g.freezer_crit_thr);
    // Switches
    get_bool(h, "sys_en",   &g.system_enable);
    get_bool(h, "door_ovr", &g.door_override);
    get_bool(h, "vac_mode", &g.vacation_mode);
    nvs_close(h);
    ESP_LOGI(TAG, "loaded config (starts=%lu runtime=%lu min)",
             (unsigned long)g.comp_starts, (unsigned long)g.comp_runtime_min);
}

void nvs_store_save(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for write failed");
        return;
    }
    {
        StateGuard lock;
        nvs_set_u32(h, "runtime_min", g.comp_runtime_min);
        nvs_set_u32(h, "starts",      g.comp_starts);
        nvs_set_u16(h, "df_comp_min", g.defrost_comp_min);
        set_f(h, "duty_pct",  g.duty_pct);
        set_f(h, "eff_value", g.eff_value);
        set_f(h, "temp_hist", g.temp_history);
        nvs_set_u8(h, "frz_warm_l",   g.freezer_warm_latched   ? 1 : 0);
        nvs_set_u8(h, "frz_cold_l",   g.freezer_cold_latched   ? 1 : 0);
        nvs_set_u8(h, "frz_unsafe_l", g.freezer_unsafe_latched ? 1 : 0);
        set_f(h, "target",      g.target_temp);
        set_f(h, "duty_on",     g.duty_on_min);
        set_f(h, "duty_off",    g.duty_off_min);
        set_f(h, "df_interval", g.defrost_interval_hours);
        set_f(h, "df_duration", g.defrost_duration_min);
        set_f(h, "vac_temp",    g.vacation_temp);
        set_f(h, "frz_warm",    g.freezer_warm_thr);
        set_f(h, "frz_cold",    g.freezer_cold_thr);
        set_f(h, "frz_crit",    g.freezer_crit_thr);
        nvs_set_u8(h, "sys_en",   g.system_enable ? 1 : 0);
        nvs_set_u8(h, "door_ovr", g.door_override ? 1 : 0);
        nvs_set_u8(h, "vac_mode", g.vacation_mode ? 1 : 0);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGD(TAG, "config committed to NVS");
}

void nvs_store_mark_dirty(void) {
    s_dirty = true;
}

static void committer_task(void *arg) {
    const TickType_t period = pdMS_TO_TICKS(5 * 60 * 1000);  // 5 min
    for (;;) {
        vTaskDelay(period);
        if (s_dirty) {
            s_dirty = false;
            nvs_store_save();
        }
    }
}

void nvs_store_task_start(void) {
    xTaskCreate(committer_task, "nvs_commit", 4096, nullptr, 2, nullptr);
}
