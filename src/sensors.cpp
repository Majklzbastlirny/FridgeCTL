#include "sensors.h"
#include "ow.h"
#include "config.h"
#include "app_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "sensors";

// ---- ADC for CT clamp ------------------------------------------
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_cali_ok = false;

void sensors_init(void) {
    // OneWire buses
    ow_init(PIN_OW_UPPER);
    ow_init(PIN_OW_LOWER);
    ow_init(PIN_OW_COMPRESSOR);
    ow_init(PIN_OW_AMBIENT);

    // ADC1 oneshot
    adc_oneshot_unit_init_cfg_t init = {};
    init.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init, &s_adc));

    adc_oneshot_chan_cfg_t ch = {};
    ch.atten    = ADC_CT_ATTEN;
    ch.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, PIN_ADC_CT_CHANNEL, &ch));

    // Voltage calibration (curve fitting where available, else line)
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cc = {};
    cc.unit_id  = ADC_UNIT_1;
    cc.chan     = PIN_ADC_CT_CHANNEL;
    cc.atten    = ADC_CT_ATTEN;
    cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cc, &s_cali) == ESP_OK) s_cali_ok = true;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t lc = {};
    lc.unit_id  = ADC_UNIT_1;
    lc.atten    = ADC_CT_ATTEN;
    lc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_line_fitting(&lc, &s_cali) == ESP_OK) s_cali_ok = true;
#endif
    if (!s_cali_ok) ESP_LOGW(TAG, "ADC calibration unavailable, using raw scaling");

    ESP_LOGI(TAG, "sensors initialised");
}

float sensors_internal_temp(void) {
    // ESP32 (classic) die-temp sensor is unreliable / unsupported on this
    // IDF target; reported as unavailable rather than wrong.
    return NAN;
}

// Read ADC channel as volts.
static inline float adc_volts(void) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, PIN_ADC_CT_CHANNEL, &raw) != ESP_OK) return NAN;
    if (s_cali_ok) {
        int mv = 0;
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
        return mv / 1000.0f;
    }
    // Fallback: assume 12-bit over ~3.3V
    return (raw / 4095.0f) * 3.3f;
}

// One CT measurement: RMS of the AC component over CT_SAMPLE_MS, then
// the linear calibration + noise-floor clamp (matches the ESPHome
// ct_clamp + calibrate_linear + lambda pipeline).
static float ct_measure(void) {
    double sum = 0, sumsq = 0;
    uint32_t n = 0;
    int64_t end = esp_timer_get_time() + (int64_t)CT_SAMPLE_MS * 1000;
    while (esp_timer_get_time() < end) {
        float v = adc_volts();
        if (!isnan(v)) { sum += v; sumsq += (double)v * v; n++; }
    }
    if (n == 0) return NAN;
    double mean = sum / n;
    double var  = sumsq / n - mean * mean;
    if (var < 0) var = 0;
    float vrms = (float)sqrt(var);                 // AC RMS in volts

    float amps = CT_CAL_SLOPE * vrms + CT_CAL_OFFSET;
    if (amps < CT_NOISE_FLOOR_A) amps = 0.0f;      // clamp noise floor
    return amps;
}

// ---- CT task: samples current; captures inrush peak ------------
static void ct_task(void *arg) {
    for (;;) {
        float amps = ct_measure();
        uint32_t now = millis();

        bool in_inrush;
        {
            StateGuard lock;
            if (!isnan(amps)) g.compressor_current = amps;
            in_inrush = g.comp_on && g.comp_start_ms != 0 &&
                        (uint32_t)(now - g.comp_start_ms) < COMP_INRUSH_WINDOW_MS;
            if (in_inrush && !isnan(amps) && amps > g.inrush_peak)
                g.inrush_peak = amps;
        }

        // During the inrush window sample back-to-back for a good peak;
        // otherwise ~1 s cadence.
        if (!in_inrush) vTaskDelay(pdMS_TO_TICKS(800));
        else            vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Fault debounce: a fresh read already retries internally (ow.cpp). On top of
// that, we only let a NAN overwrite the published value after several
// CONSECUTIVE failed cycles — so a single bad cycle holds the last good value
// and never surfaces as a sensor fault. A genuinely dead sensor still trips
// within MISS_LIMIT cycles (~1.5 min at 30 s cadence).
#define MISS_LIMIT 3

// Commit a reading with debounce. Updates *out only on success or once the
// miss counter passes the limit (then publishes NAN = real fault). The
// per-sensor calibration offset is applied to a valid reading here so every
// downstream consumer (control, MQTT, web) sees the corrected value; a NAN
// (fault) is never shifted.
static void commit(float reading, float offset, float *out, uint8_t *miss) {
    if (!isnan(reading)) {
        *out = reading + offset;
        *miss = 0;
    } else if (*miss < MISS_LIMIT) {
        (*miss)++;             // hold last good value this cycle
    } else {
        *out = NAN;            // sustained failure -> report fault
    }
}

// ---- Temperature task ------------------------------------------
static void temp_task(void *arg) {
    uint8_t cycle = 0;
    uint8_t miss_u = 0, miss_l = 0, miss_c = 0, miss_a = 0, miss_f = 0;
    for (;;) {
        bool ambient_cycle = (cycle % DS18B20_AMBIENT_DIV) == 0;

        // Kick off conversions on all buses simultaneously, then wait once.
        ds18b20_convert_all(PIN_OW_UPPER);
        ds18b20_convert_all(PIN_OW_LOWER);
        ds18b20_convert_all(PIN_OW_COMPRESSOR);
        if (ambient_cycle) ds18b20_convert_all(PIN_OW_AMBIENT);

        vTaskDelay(pdMS_TO_TICKS(800));   // 12-bit conversion needs ~750 ms

        float tu = ds18b20_read_single(PIN_OW_UPPER);
        float tl = ds18b20_read_single(PIN_OW_LOWER);
        float tc = ds18b20_read_single(PIN_OW_COMPRESSOR);

        float ta = NAN, tf = NAN;
        if (ambient_cycle) {
            ta = ds18b20_read_addr(PIN_OW_AMBIENT, ADDR_AMBIENT);
            tf = ds18b20_read_addr(PIN_OW_AMBIENT, ADDR_FREEZER);
        }

        {
            StateGuard lock;
            commit(tu, g.off_upper,      &g.temp_upper,      &miss_u);
            commit(tl, g.off_lower,      &g.temp_lower,      &miss_l);
            commit(tc, g.off_compressor, &g.temp_compressor, &miss_c);
            if (ambient_cycle) {
                commit(ta, g.off_ambient, &g.temp_ambient, &miss_a);
                commit(tf, g.off_freezer, &g.temp_freezer, &miss_f);
            }
        }

        cycle++;
        // Total cadence ~30 s (already spent ~0.8 s converting).
        vTaskDelay(pdMS_TO_TICKS(DS18B20_PERIOD_MS - 800));
    }
}

void sensors_temp_task_start(void) {
    xTaskCreate(temp_task, "temp", 4096, nullptr, 4, nullptr);
}

void sensors_ct_task_start(void) {
    xTaskCreate(ct_task, "ct", 4096, nullptr, 4, nullptr);
}
