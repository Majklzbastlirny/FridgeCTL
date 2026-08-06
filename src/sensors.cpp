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

// ---- 1-Wire bus scan -------------------------------------------
static const gpio_num_t OW_PINS[OW_BUS_COUNT] = {
    PIN_OW_UPPER, PIN_OW_LOWER, PIN_OW_COMPRESSOR, PIN_OW_AMBIENT
};
static const OwBusInfo OW_BUSES[OW_BUS_COUNT] = {
    { "Upper chamber", (uint8_t)PIN_OW_UPPER,      "upper"      },
    { "Lower chamber", (uint8_t)PIN_OW_LOWER,      "lower"      },
    { "Compressor",    (uint8_t)PIN_OW_COMPRESSOR, "compressor" },
    { "Shared bus",    (uint8_t)PIN_OW_AMBIENT,    ""           },  // ambient + freezer
};

const OwBusInfo *sensors_bus_info(int bus) {
    if (bus < 0 || bus >= OW_BUS_COUNT) return nullptr;
    return &OW_BUSES[bus];
}

static volatile bool s_scan_req    = true;   // scan once on the very first cycle
static volatile bool s_scanning    = false;
static volatile bool s_roles_dirty = false;
static OwFound       s_found[OW_SCAN_MAX];   // all four guarded by the state lock
static int           s_found_n     = 0;
static uint32_t      s_scan_ms     = 0;
static bool          s_scan_done   = false;

void sensors_request_scan(void)  { s_scan_req = true; }
bool sensors_scan_pending(void)  { return s_scan_req || s_scanning; }
bool sensors_scan_done(void)     { StateGuard lock; return s_scan_done; }
void sensors_roles_changed(void) { s_roles_dirty = true; }

int sensors_scan_results(OwFound *out, int max, uint32_t *age_ms) {
    StateGuard lock;
    int n = s_found_n < max ? s_found_n : max;
    for (int i = 0; i < n; i++) out[i] = s_found[i];
    if (age_ms) *age_ms = s_scan_done ? (uint32_t)(millis() - s_scan_ms) : 0;
    return n;
}

// Enumerate every bus, then read each device found by its own ROM. Costs one
// shared 750 ms conversion window (~1 s total). Runs only from temp_task:
// the bit-banged bus has no task-level mutex, so the HTTP handler must never
// touch it — it only raises s_scan_req.
static void do_scan(void) {
    OwFound found[OW_SCAN_MAX];
    int n = 0;

    for (int b = 0; b < OW_BUS_COUNT; b++) {
        uint64_t roms[OW_MAX_DEVICES];
        int cnt = ow_search(OW_PINS[b], roms, OW_MAX_DEVICES);
        for (int i = 0; i < cnt && n < OW_SCAN_MAX; i++) {
            found[n].bus  = (uint8_t)b;
            found[n].rom  = roms[i];
            found[n].temp = NAN;
            n++;
        }
        ds18b20_convert_all(OW_PINS[b]);   // all buses convert in parallel
    }

    vTaskDelay(pdMS_TO_TICKS(800));        // 12-bit conversion needs ~750 ms

    for (int i = 0; i < n; i++)
        found[i].temp = ds18b20_read_addr(OW_PINS[found[i].bus], found[i].rom);

    {
        StateGuard lock;
        for (int i = 0; i < n; i++) s_found[i] = found[i];
        s_found_n   = n;
        s_scan_ms   = millis();
        s_scan_done = true;
    }
    ESP_LOGI(TAG, "1-Wire scan: %d device(s) found across %d buses", n, OW_BUS_COUNT);
}

// ---- Temperature task ------------------------------------------
static void temp_task(void *arg) {
    uint8_t cycle = 0;
    uint8_t miss_u = 0, miss_l = 0, miss_c = 0, miss_a = 0, miss_f = 0;
    for (;;) {
        if (s_scan_req) {
            s_scan_req = false;   // cleared first: a request raised *during*
            s_scanning = true;    // the scan queues another one
            do_scan();
            s_scanning = false;
        }

        if (s_roles_dirty) {
            // The ambient/freezer ROM assignment changed. Drop the debounce
            // state so commit() can't keep publishing a held value that
            // actually belongs to the other probe.
            s_roles_dirty = false;
            miss_a = miss_f = MISS_LIMIT;
            StateGuard lock;
            g.temp_ambient = NAN;
            g.temp_freezer = NAN;
        }

        bool ambient_cycle = (cycle % DS18B20_AMBIENT_DIV) == 0;

        // Which ROM plays which role on the shared bus. 0 = never assigned,
        // so fall back to the known-good compile-time default.
        uint64_t rom_amb, rom_frz;
        {
            StateGuard lock;
            rom_amb = g.addr_ambient ? g.addr_ambient : ADDR_AMBIENT;
            rom_frz = g.addr_freezer ? g.addr_freezer : ADDR_FREEZER;
        }

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
            ta = ds18b20_read_addr(PIN_OW_AMBIENT, rom_amb);
            tf = ds18b20_read_addr(PIN_OW_AMBIENT, rom_frz);
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
        // Total cadence ~30 s (already spent ~0.8 s converting). Waited in
        // 1 s slices so a web-triggered rescan starts within ~1 s instead of
        // sitting out the rest of the cycle.
        for (uint32_t left = DS18B20_PERIOD_MS - 800; left > 0; ) {
            uint32_t slice = left > 1000 ? 1000 : left;
            vTaskDelay(pdMS_TO_TICKS(slice));
            left -= slice;
            if (s_scan_req) break;
        }
    }
}

void sensors_temp_task_start(void) {
    // 5120: the scan builds an OwFound[OW_SCAN_MAX] snapshot on this stack.
    xTaskCreate(temp_task, "temp", 5120, nullptr, 4, nullptr);
}

void sensors_ct_task_start(void) {
    xTaskCreate(ct_task, "ct", 4096, nullptr, 4, nullptr);
}
