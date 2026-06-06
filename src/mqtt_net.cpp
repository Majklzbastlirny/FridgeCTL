#include "mqtt_net.h"
#include "config.h"
#include "secrets.h"
#include "app_state.h"
#include "control.h"
#include "gpio_io.h"
#include "sensors.h"
#include "nvs_store.h"
#include "diag.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>   // strncasecmp
#include <stdio.h>
#include <math.h>

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client = nullptr;
static volatile bool s_connected = false;

#define AVAIL_TOPIC   DEVICE_ID "/status"

// Shared HA device descriptor injected into every discovery config.
#define DEV_JSON \
    "\"dev\":{\"ids\":[\"" DEVICE_ID "\"],\"name\":\"" DEVICE_NAME \
    "\",\"mdl\":\"LilyGo T-Relay\",\"mf\":\"DIY\",\"sw\":\"" FW_VERSION "\"}"

// ================================================================
//  Getters / setters (each locks state as needed)
// ================================================================
#define FGET(n, expr) static float fg_##n(void){ StateGuard l; return (expr); }
#define BGET(n, expr) static bool  bg_##n(void){ StateGuard l; return (expr); }

// --- sensor getters ---
FGET(temp_upper,      g.temp_upper)
FGET(temp_lower,      g.temp_lower)
FGET(temp_comp,       g.temp_compressor)
FGET(temp_ambient,    g.temp_ambient)
FGET(temp_freezer,    g.temp_freezer)
FGET(current,         g.compressor_current)
FGET(inrush,          g.inrush_peak)
FGET(runtime_h,       g.comp_runtime_min / 60.0f)
FGET(starts,          (float)g.comp_starts)
FGET(duty_pct,        g.duty_pct * 100.0f)
static float fg_control_temp(void){ return ctl_control_temp(); }
static float fg_efficiency(void) { return ctl_efficiency(); }
static float fg_rssi(void){ StateGuard l; return (float)g.wifi_rssi; }
static float fg_uptime(void){ return millis() / 1000.0f; }
static float fg_heap_free(void){ return (float)esp_get_free_heap_size(); }
static float fg_heap_block(void){ return (float)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }

// --- binary getters ---
BGET(comp_running,  g.comp_on)
BGET(fan_running,   g.fan_on)
BGET(comp_overtemp, g.comp_overtemp)
BGET(overcurrent,   g.overcurrent_fault)
BGET(freezer_warm,  g.freezer_warm_latched)
BGET(freezer_cold,  g.freezer_cold_latched)
BGET(freezer_unsafe,g.freezer_unsafe_latched)
BGET(ct_fault,      g.no_current_count >= CT_NO_CURRENT_COUNT)
BGET(cap_fault,     g.intermittent_zeros >= CAP_FAULT_COUNT)
BGET(defrost,       g.defrost_active)
BGET(door,          g.door_phys)
static bool bg_blind(void){ return ctl_blind_mode(); }
static bool bg_comp_temp_fault(void){ return ctl_comp_temp_fault(); }
static bool bg_freezer_sensor_fault(void){ return ctl_freezer_sensor_fault(); }
static bool bg_food_safety(void){ return ctl_food_unsafe(); }
static bool bg_initial_cooldown(void){ return ctl_initial_cooldown(); }
static bool bg_ota_in_progress(void){ StateGuard l; return g.ota_in_progress; }
static bool bg_ota_failed(void){ StateGuard l; return g.ota_failed; }
static bool bg_temp_rising(void){ return ctl_temp_rising(); }
static bool bg_high_duty(void){ return ctl_high_duty(); }

// --- switch getters/setters ---
static bool sw_get_sysen(void){ StateGuard l; return g.system_enable; }
static void sw_set_sysen(bool v){ StateGuard l; g.system_enable = v; nvs_store_mark_dirty(); }
static bool sw_get_turbo(void){ return ctl_turbo(); }
static void sw_set_turbo(bool v){ ctl_set_turbo(v); }
static bool sw_get_doorovr(void){ StateGuard l; return g.door_override; }
static void sw_set_doorovr(bool v){ StateGuard l; g.door_override = v; nvs_store_mark_dirty(); }
static bool sw_get_vac(void){ StateGuard l; return g.vacation_mode; }
static void sw_set_vac(bool v){ StateGuard l; g.vacation_mode = v; nvs_store_mark_dirty(); }
static bool sw_get_k1(void){ StateGuard l; return g.k1_on; }
static void sw_set_k1(bool v){ StateGuard l; g.k1_on = v; relay_k1(v); }

// --- number getters/setters ---
#define NSET(n, field, lo, hi) \
    static void ns_##n(float v){ if(v<lo)v=lo; if(v>hi)v=hi; StateGuard l; g.field=v; nvs_store_mark_dirty(); } \
    static float ng_##n(void){ StateGuard l; return g.field; }
NSET(target,     target_temp,            1.0f,  10.0f)
NSET(duty_on,    duty_on_min,            5.0f,  60.0f)
NSET(duty_off,   duty_off_min,           5.0f,  60.0f)
NSET(df_int,     defrost_interval_hours, 4.0f,  24.0f)
NSET(df_dur,     defrost_duration_min,   5.0f,  30.0f)
NSET(vac_temp,   vacation_temp,          5.0f,  12.0f)
NSET(frz_warm,   freezer_warm_thr,     -20.0f,   0.0f)
NSET(frz_cold,   freezer_cold_thr,     -40.0f, -15.0f)
NSET(frz_crit,   freezer_crit_thr,     -10.0f,  10.0f)

// --- text getters ---
static void tg_ip(char *b, size_t n){ StateGuard l; strlcpy(b, g.ip_addr, n); }
static void tg_ssid(char *b, size_t n){ StateGuard l; strlcpy(b, g.ssid, n); }
static void tg_mac(char *b, size_t n){ StateGuard l; strlcpy(b, g.mac_addr, n); }
static void tg_alarm(char *b, size_t n){ std::string s = ctl_alarm_state(); strlcpy(b, s.c_str(), n); }
static void tg_devinfo(char *b, size_t n){ strlcpy(b, DEVICE_NAME " " FW_VERSION " (ESP32)", n); }
static void tg_reset_counts(char *b, size_t n){ diag_reset_counts(b, n); }
static void tg_last_trace(char *b, size_t n){ diag_last_trace(b, n); }
static void tg_reset(char *b, size_t n){
    const char *r;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  r = "Power on";       break;
        case ESP_RST_SW:       r = "Software";       break;
        case ESP_RST_PANIC:    r = "Panic";          break;
        case ESP_RST_INT_WDT:  r = "Int WDT";        break;
        case ESP_RST_TASK_WDT: r = "Task WDT";       break;
        case ESP_RST_WDT:      r = "Other WDT";      break;
        case ESP_RST_DEEPSLEEP:r = "Deep sleep";     break;
        case ESP_RST_BROWNOUT: r = "Brownout";       break;
        default:               r = "Unknown";        break;
    }
    strlcpy(b, r, n);
}

// --- button actions ---
static void btn_reset_oc(void){ ctl_reset_overcurrent(); }
static void btn_defrost(void){ ctl_trigger_defrost(); }
static void btn_restart(void){ ESP_LOGW(TAG, "Restart requested via MQTT"); vTaskDelay(pdMS_TO_TICKS(200)); esp_restart(); }

// ================================================================
//  Entity tables
// ================================================================
struct SensorEnt { const char *obj, *name, *unit, *dclass, *icon; int dec; bool diag; float (*get)(void); };
struct BinEnt    { const char *obj, *name, *dclass, *icon; bool (*get)(void); };
struct SwitchEnt { const char *obj, *name, *icon; bool (*get)(void); void (*set)(bool); };
struct NumberEnt { const char *obj, *name, *unit, *icon; float mn, mx, st; bool cfg; float (*get)(void); void (*set)(float); };
struct ButtonEnt { const char *obj, *name, *icon; void (*press)(void); };
struct TextEnt   { const char *obj, *name, *icon; void (*get)(char*, size_t); };

static const SensorEnt SENSORS[] = {
  {"upper_temp","Upper Temp","°C","temperature",nullptr,1,false,fg_temp_upper},
  {"lower_temp","Lower Temp","°C","temperature",nullptr,1,false,fg_temp_lower},
  {"compressor_temp","Compressor Temp","°C","temperature",nullptr,1,false,fg_temp_comp},
  {"ambient_temp","Ambient Temp","°C","temperature",nullptr,1,false,fg_temp_ambient},
  {"freezer_temp","Freezer Temp","°C","temperature",nullptr,1,false,fg_temp_freezer},
  {"compressor_current","Compressor Current","A","current",nullptr,2,false,fg_current},
  {"control_temp","Control Temperature","°C","temperature","mdi:thermometer",1,false,fg_control_temp},
  {"compressor_runtime","Compressor Runtime","h",nullptr,"mdi:clock-outline",1,false,fg_runtime_h},
  {"compressor_starts","Compressor Starts",nullptr,nullptr,"mdi:counter",0,false,fg_starts},
  {"duty_cycle","Compressor Duty Cycle","%",nullptr,"mdi:percent",0,false,fg_duty_pct},
  {"cooling_efficiency","Cooling Efficiency","°C/min",nullptr,"mdi:thermometer-chevron-down",3,false,fg_efficiency},
  {"inrush_peak","Last Inrush Peak","A","current","mdi:lightning-bolt",2,false,fg_inrush},
  {"wifi_rssi","WiFi Signal","dBm","signal_strength",nullptr,0,true,fg_rssi},
  {"uptime","Uptime","s","duration",nullptr,0,true,fg_uptime},
  {"heap_free","Heap Free","B","data_size",nullptr,0,true,fg_heap_free},
  {"heap_block","Heap Max Block","B","data_size",nullptr,0,true,fg_heap_block},
};

static const BinEnt BINS[] = {
  {"compressor_running","Compressor Running","running",nullptr,bg_comp_running},
  {"fan_running","Fan Running","running",nullptr,bg_fan_running},
  {"compressor_overtemp","Compressor Overtemp","heat",nullptr,bg_comp_overtemp},
  {"overcurrent_fault","Overcurrent Fault","problem",nullptr,bg_overcurrent},
  {"blind_duty","Blind Duty Cycle Active","problem","mdi:thermometer-off",bg_blind},
  {"comp_temp_fault","Compressor Temp Sensor Fault","problem","mdi:thermometer-alert",bg_comp_temp_fault},
  {"freezer_sensor_fault","Freezer Sensor Fault","problem","mdi:thermometer-alert",bg_freezer_sensor_fault},
  {"freezer_warm","Freezer Too Warm","problem","mdi:thermometer-high",bg_freezer_warm},
  {"freezer_cold","Freezer Too Cold","problem","mdi:thermometer-low",bg_freezer_cold},
  {"freezer_food_safety","Freezer Food Safety Alert","safety","mdi:food-off",bg_freezer_unsafe},
  {"ct_fault","CT Sensor Fault","problem","mdi:flash-alert",bg_ct_fault},
  {"cap_fault","Possible Capacitor Fault","problem","mdi:capacitor",bg_cap_fault},
  {"food_safety","Food Safety Alert","safety","mdi:food-off",bg_food_safety},
  {"defrost_active","Defrost Active",nullptr,"mdi:snowflake-melt",bg_defrost},
  {"initial_cooldown","Initial Cooldown",nullptr,"mdi:thermometer-chevron-down",bg_initial_cooldown},
  {"ota_in_progress","Firmware Update In Progress","update","mdi:cloud-download",bg_ota_in_progress},
  {"ota_failed","Firmware Update Failed","problem","mdi:cloud-alert",bg_ota_failed},
  {"temp_rising","Temp Rising While Cooling","problem","mdi:thermometer-alert",bg_temp_rising},
  {"high_duty","High Duty Cycle","problem","mdi:gauge-full",bg_high_duty},
  {"door","Door","door",nullptr,bg_door},
};

static const SwitchEnt SWITCHES[] = {
  {"system_enable","System Enable","mdi:power",sw_get_sysen,sw_set_sysen},
  {"turbo","Turbo Mode","mdi:snowflake-alert",sw_get_turbo,sw_set_turbo},
  {"door_override","Door Override","mdi:door-closed-lock",sw_get_doorovr,sw_set_doorovr},
  {"vacation_mode","Vacation Mode","mdi:beach",sw_get_vac,sw_set_vac},
  {"k1_spare","K1 Spare","mdi:toggle-switch",sw_get_k1,sw_set_k1},
};

static const NumberEnt NUMBERS[] = {
  {"target_temp","Target Temperature","°C","mdi:thermometer-chevron-down",1.0f,10.0f,0.5f,false,ng_target,ns_target},
  {"duty_on_min","Duty Cycle ON Minutes","min","mdi:timer-play",5.0f,60.0f,5.0f,true,ng_duty_on,ns_duty_on},
  {"duty_off_min","Duty Cycle OFF Minutes","min","mdi:timer-pause",5.0f,60.0f,5.0f,true,ng_duty_off,ns_duty_off},
  {"defrost_interval","Defrost Interval","h","mdi:snowflake-melt",4.0f,24.0f,1.0f,true,ng_df_int,ns_df_int},
  {"defrost_duration","Defrost Duration","min","mdi:snowflake-melt",5.0f,30.0f,5.0f,true,ng_df_dur,ns_df_dur},
  {"vacation_temp","Vacation Temperature","°C","mdi:beach",5.0f,12.0f,0.5f,true,ng_vac_temp,ns_vac_temp},
  {"freezer_warm_thr","Freezer Warm Threshold","°C","mdi:thermometer-high",-20.0f,0.0f,0.5f,true,ng_frz_warm,ns_frz_warm},
  {"freezer_cold_thr","Freezer Cold Threshold","°C","mdi:thermometer-low",-40.0f,-15.0f,1.0f,true,ng_frz_cold,ns_frz_cold},
  {"freezer_crit_thr","Freezer Critical Threshold","°C","mdi:food-off",-10.0f,10.0f,1.0f,true,ng_frz_crit,ns_frz_crit},
};

static const ButtonEnt BUTTONS[] = {
  {"reset_overcurrent","Reset Overcurrent Fault","mdi:alert-remove",btn_reset_oc},
  {"trigger_defrost","Trigger Defrost","mdi:snowflake-melt",btn_defrost},
  {"restart","Restart","mdi:restart",btn_restart},
};

static const TextEnt TEXTS[] = {
  {"ip_address","IP Address","mdi:ip",tg_ip},
  {"wifi_ssid","WiFi SSID","mdi:wifi",tg_ssid},
  {"mac_address","MAC Address","mdi:network",tg_mac},
  {"alarm_state","Alarm State","mdi:alert-circle",tg_alarm},
  {"reset_reason","Reset Reason","mdi:restart-alert",tg_reset},
  {"device_info","Device Info","mdi:information",tg_devinfo},
  {"reset_counts","Reset Counts","mdi:counter",tg_reset_counts},
  {"pre_reset_trace","Pre-Reset Trace","mdi:history",tg_last_trace},
};

// ================================================================
//  Topic helpers
// ================================================================
static void state_topic(char *b, size_t n, const char *obj) {
    snprintf(b, n, DEVICE_ID "/%s/state", obj);
}
static void cmd_topic(char *b, size_t n, const char *obj) {
    snprintf(b, n, DEVICE_ID "/%s/cmd", obj);
}
static void disc_topic(char *b, size_t n, const char *comp, const char *obj) {
    snprintf(b, n, DISCOVERY_PREFIX "/%s/" DEVICE_ID "/%s/config", comp, obj);
}

static void pub(const char *topic, const char *payload, int retain) {
    if (!s_client || !s_connected) return;
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, retain);
}

// ================================================================
//  Discovery publishing
// ================================================================
static void publish_discovery(void) {
    char topic[160];
    char st[96], cmd[96];
    static char cfg[768];

    // Sensors
    for (auto &e : SENSORS) {
        disc_topic(topic, sizeof(topic), "sensor", e.obj);
        state_topic(st, sizeof(st), e.obj);
        int p = snprintf(cfg, sizeof(cfg),
            "{\"name\":\"%s\",\"uniq_id\":\"" DEVICE_ID "_%s\",\"stat_t\":\"%s\","
            "\"avty_t\":\"" AVAIL_TOPIC "\"", e.name, e.obj, st);
        if (e.unit)   p += snprintf(cfg+p, sizeof(cfg)-p, ",\"unit_of_meas\":\"%s\"", e.unit);
        if (e.dclass) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"dev_cla\":\"%s\"", e.dclass);
        if (e.icon)   p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ic\":\"%s\"", e.icon);
        if (e.diag)   p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ent_cat\":\"diagnostic\"");
        snprintf(cfg+p, sizeof(cfg)-p, ",%s}", DEV_JSON);
        pub(topic, cfg, 1);
    }

    // Binary sensors
    for (auto &e : BINS) {
        disc_topic(topic, sizeof(topic), "binary_sensor", e.obj);
        state_topic(st, sizeof(st), e.obj);
        int p = snprintf(cfg, sizeof(cfg),
            "{\"name\":\"%s\",\"uniq_id\":\"" DEVICE_ID "_%s\",\"stat_t\":\"%s\","
            "\"avty_t\":\"" AVAIL_TOPIC "\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"",
            e.name, e.obj, st);
        if (e.dclass) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"dev_cla\":\"%s\"", e.dclass);
        if (e.icon)   p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ic\":\"%s\"", e.icon);
        snprintf(cfg+p, sizeof(cfg)-p, ",%s}", DEV_JSON);
        pub(topic, cfg, 1);
    }

    // Switches
    for (auto &e : SWITCHES) {
        disc_topic(topic, sizeof(topic), "switch", e.obj);
        state_topic(st, sizeof(st), e.obj);
        cmd_topic(cmd, sizeof(cmd), e.obj);
        int p = snprintf(cfg, sizeof(cfg),
            "{\"name\":\"%s\",\"uniq_id\":\"" DEVICE_ID "_%s\",\"stat_t\":\"%s\","
            "\"cmd_t\":\"%s\",\"avty_t\":\"" AVAIL_TOPIC "\",\"pl_on\":\"ON\",\"pl_off\":\"OFF\"",
            e.name, e.obj, st, cmd);
        if (e.icon) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ic\":\"%s\"", e.icon);
        snprintf(cfg+p, sizeof(cfg)-p, ",%s}", DEV_JSON);
        pub(topic, cfg, 1);
    }

    // Numbers
    for (auto &e : NUMBERS) {
        disc_topic(topic, sizeof(topic), "number", e.obj);
        state_topic(st, sizeof(st), e.obj);
        cmd_topic(cmd, sizeof(cmd), e.obj);
        int p = snprintf(cfg, sizeof(cfg),
            "{\"name\":\"%s\",\"uniq_id\":\"" DEVICE_ID "_%s\",\"stat_t\":\"%s\","
            "\"cmd_t\":\"%s\",\"avty_t\":\"" AVAIL_TOPIC "\",\"min\":%.2f,\"max\":%.2f,"
            "\"step\":%.2f,\"mode\":\"box\"",
            e.name, e.obj, st, cmd, e.mn, e.mx, e.st);
        if (e.unit) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"unit_of_meas\":\"%s\"", e.unit);
        if (e.icon) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ic\":\"%s\"", e.icon);
        if (e.cfg)  p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ent_cat\":\"config\"");
        snprintf(cfg+p, sizeof(cfg)-p, ",%s}", DEV_JSON);
        pub(topic, cfg, 1);
    }

    // Buttons
    for (auto &e : BUTTONS) {
        disc_topic(topic, sizeof(topic), "button", e.obj);
        cmd_topic(cmd, sizeof(cmd), e.obj);
        int p = snprintf(cfg, sizeof(cfg),
            "{\"name\":\"%s\",\"uniq_id\":\"" DEVICE_ID "_%s\",\"cmd_t\":\"%s\","
            "\"avty_t\":\"" AVAIL_TOPIC "\",\"ent_cat\":\"config\"", e.name, e.obj, cmd);
        if (e.icon) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ic\":\"%s\"", e.icon);
        snprintf(cfg+p, sizeof(cfg)-p, ",%s}", DEV_JSON);
        pub(topic, cfg, 1);
    }

    // Text sensors
    for (auto &e : TEXTS) {
        disc_topic(topic, sizeof(topic), "sensor", e.obj);
        state_topic(st, sizeof(st), e.obj);
        int p = snprintf(cfg, sizeof(cfg),
            "{\"name\":\"%s\",\"uniq_id\":\"" DEVICE_ID "_%s\",\"stat_t\":\"%s\","
            "\"avty_t\":\"" AVAIL_TOPIC "\",\"ent_cat\":\"diagnostic\"", e.name, e.obj, st);
        if (e.icon) p += snprintf(cfg+p, sizeof(cfg)-p, ",\"ic\":\"%s\"", e.icon);
        snprintf(cfg+p, sizeof(cfg)-p, ",%s}", DEV_JSON);
        pub(topic, cfg, 1);
    }

    // Interior light (JSON schema with brightness)
    {
        disc_topic(topic, sizeof(topic), "light", "interior_light");
        state_topic(st, sizeof(st), "interior_light");
        cmd_topic(cmd, sizeof(cmd), "interior_light");
        snprintf(cfg, sizeof(cfg),
            "{\"name\":\"Interior Light\",\"uniq_id\":\"" DEVICE_ID "_interior_light\","
            "\"schema\":\"json\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
            "\"avty_t\":\"" AVAIL_TOPIC "\",\"brightness\":true,%s}", st, cmd, DEV_JSON);
        pub(topic, cfg, 1);
    }
    ESP_LOGI(TAG, "HA discovery published");
}

// ================================================================
//  State publishing
// ================================================================
void mqtt_publish_all_states(void) {
    if (!s_connected) return;
    char st[96], val[64];

    for (auto &e : SENSORS) {
        float v = e.get();
        if (isnan(v)) continue;             // leave HA value as last-known
        state_topic(st, sizeof(st), e.obj);
        snprintf(val, sizeof(val), "%.*f", e.dec, v);
        pub(st, val, 0);
    }
    for (auto &e : BINS) {
        state_topic(st, sizeof(st), e.obj);
        pub(st, e.get() ? "ON" : "OFF", 0);
    }
    for (auto &e : SWITCHES) {
        state_topic(st, sizeof(st), e.obj);
        pub(st, e.get() ? "ON" : "OFF", 0);
    }
    for (auto &e : NUMBERS) {
        state_topic(st, sizeof(st), e.obj);
        snprintf(val, sizeof(val), "%.2f", e.get());
        pub(st, val, 0);
    }
    for (auto &e : TEXTS) {
        state_topic(st, sizeof(st), e.obj);
        char buf[64];
        e.get(buf, sizeof(buf));
        pub(st, buf, 0);
    }
    // Interior light JSON state
    {
        bool on; int bri;
        { StateGuard l; on = g.light_on; bri = (int)(g.light_brightness * 255.0f + 0.5f); }
        state_topic(st, sizeof(st), "interior_light");
        snprintf(val, sizeof(val), "{\"state\":\"%s\",\"brightness\":%d}",
                 on ? "ON" : "OFF", bri);
        pub(st, val, 0);
    }
}

// ================================================================
//  Command routing
// ================================================================
static bool topic_match(const char *topic, int tlen, const char *obj) {
    char t[96];
    cmd_topic(t, sizeof(t), obj);
    return (int)strlen(t) == tlen && strncmp(topic, t, tlen) == 0;
}

static void handle_command(const char *topic, int tlen, const char *data, int dlen) {
    char payload[64];
    int n = dlen < (int)sizeof(payload) - 1 ? dlen : (int)sizeof(payload) - 1;
    memcpy(payload, data, n);
    payload[n] = 0;
    bool on = (strncasecmp(payload, "ON", 2) == 0) || strcmp(payload, "1") == 0;

    for (auto &e : SWITCHES)
        if (topic_match(topic, tlen, e.obj)) { e.set(on); mqtt_publish_all_states(); return; }
    for (auto &e : NUMBERS)
        if (topic_match(topic, tlen, e.obj)) { e.set(strtof(payload, nullptr)); mqtt_publish_all_states(); return; }
    for (auto &e : BUTTONS)
        if (topic_match(topic, tlen, e.obj)) { e.press(); return; }

    // Interior light (JSON command)
    if (topic_match(topic, tlen, "interior_light")) {
        bool want_on  = strstr(payload, "\"state\":\"ON\"")  != nullptr;
        bool want_off = strstr(payload, "\"state\":\"OFF\"") != nullptr;
        float bri = -1.0f;
        const char *bp = strstr(payload, "\"brightness\":");
        if (bp) bri = strtof(bp + 13, nullptr) / 255.0f;
        {
            StateGuard l;
            if (bri >= 0.0f) g.light_brightness = bri;
            if (want_off) g.light_on = false; else if (want_on) g.light_on = true;
            light_set(g.light_on, g.light_brightness);
        }
        mqtt_publish_all_states();
        return;
    }
    ESP_LOGW(TAG, "unhandled command topic");
}

// ================================================================
//  MQTT event handler
// ================================================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data) {
    auto *event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        { StateGuard l; g.mqtt_connected = true; }
        esp_mqtt_client_publish(s_client, AVAIL_TOPIC, "online", 0, 1, 1);
        esp_mqtt_client_subscribe(s_client, DEVICE_ID "/+/cmd", 0);
        publish_discovery();
        mqtt_publish_all_states();
        ESP_LOGI(TAG, "connected, subscribed to commands");
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        { StateGuard l; g.mqtt_connected = false; }
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_command(event->topic, event->topic_len, event->data, event->data_len);
        break;
    default:
        break;
    }
}

void mqtt_net_start(void) {
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = MQTT_URI;
#if defined(MQTT_USERNAME)
    if (strlen(MQTT_USERNAME) > 0) {
        cfg.credentials.username = MQTT_USERNAME;
        cfg.credentials.authentication.password = MQTT_PASSWORD;
    }
#endif
    cfg.session.last_will.topic   = AVAIL_TOPIC;
    cfg.session.last_will.msg     = "offline";
    cfg.session.last_will.qos     = 1;
    cfg.session.last_will.retain  = 1;
    cfg.session.keepalive         = 30;

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "client started -> %s", MQTT_URI);
}

// ================================================================
//  Web API — reuses the same entity tables as MQTT so the web UI and
//  Home Assistant always expose an identical surface.
// ================================================================

// Append a JSON string value with minimal escaping (\\ and ").
static int json_str(char *b, size_t n, const char *s) {
    int p = 0;
    if (p < (int)n) b[p++] = '"';
    for (; *s && p < (int)n - 2; s++) {
        if (*s == '"' || *s == '\\') b[p++] = '\\';
        b[p++] = *s;
    }
    if (p < (int)n) b[p++] = '"';
    return p;
}

// Build the full device state as JSON into buf. Used by the read-only
// web dashboard (and the control page). Returns bytes written.
int web_build_state_json(char *buf, size_t n) {
    int p = 0;
    p += snprintf(buf + p, n - p, "{\"sensors\":{");
    bool first = true;
    for (auto &e : SENSORS) {
        float v = e.get();
        p += snprintf(buf + p, n - p, "%s\"%s\":{\"n\":", first ? "" : ",", e.obj);
        p += json_str(buf + p, n - p, e.name);
        if (isnan(v)) p += snprintf(buf + p, n - p, ",\"v\":null");
        else          p += snprintf(buf + p, n - p, ",\"v\":%.*f", e.dec, v);
        p += snprintf(buf + p, n - p, ",\"u\":");
        p += json_str(buf + p, n - p, e.unit ? e.unit : "");
        p += snprintf(buf + p, n - p, "}");
        first = false;
    }
    p += snprintf(buf + p, n - p, "},\"binary\":{");
    first = true;
    for (auto &e : BINS) {
        p += snprintf(buf + p, n - p, "%s\"%s\":{\"n\":", first ? "" : ",", e.obj);
        p += json_str(buf + p, n - p, e.name);
        p += snprintf(buf + p, n - p, ",\"v\":%s}", e.get() ? "true" : "false");
        first = false;
    }
    p += snprintf(buf + p, n - p, "},\"switches\":{");
    first = true;
    for (auto &e : SWITCHES) {
        p += snprintf(buf + p, n - p, "%s\"%s\":{\"n\":", first ? "" : ",", e.obj);
        p += json_str(buf + p, n - p, e.name);
        p += snprintf(buf + p, n - p, ",\"v\":%s}", e.get() ? "true" : "false");
        first = false;
    }
    p += snprintf(buf + p, n - p, "},\"numbers\":{");
    first = true;
    for (auto &e : NUMBERS) {
        p += snprintf(buf + p, n - p, "%s\"%s\":{\"n\":", first ? "" : ",", e.obj);
        p += json_str(buf + p, n - p, e.name);
        p += snprintf(buf + p, n - p, ",\"v\":%.2f,\"min\":%.2f,\"max\":%.2f,\"step\":%.2f,\"u\":",
                      e.get(), e.mn, e.mx, e.st);
        p += json_str(buf + p, n - p, e.unit ? e.unit : "");
        p += snprintf(buf + p, n - p, "}");
        first = false;
    }
    p += snprintf(buf + p, n - p, "},\"buttons\":{");
    first = true;
    for (auto &e : BUTTONS) {
        p += snprintf(buf + p, n - p, "%s\"%s\":", first ? "" : ",", e.obj);
        p += json_str(buf + p, n - p, e.name);
        first = false;
    }
    p += snprintf(buf + p, n - p, "},\"text\":{");
    first = true;
    for (auto &e : TEXTS) {
        char tb[64];
        e.get(tb, sizeof(tb));
        p += snprintf(buf + p, n - p, "%s\"%s\":{\"n\":", first ? "" : ",", e.obj);
        p += json_str(buf + p, n - p, e.name);
        p += snprintf(buf + p, n - p, ",\"v\":");
        p += json_str(buf + p, n - p, tb);
        p += snprintf(buf + p, n - p, "}");
        first = false;
    }
    p += snprintf(buf + p, n - p, "}}");
    return p;
}

// Control routing for the web control page. Return true if handled.
bool web_set_switch(const char *obj, bool on) {
    for (auto &e : SWITCHES)
        if (strcmp(e.obj, obj) == 0) { e.set(on); mqtt_publish_all_states(); return true; }
    return false;
}
bool web_set_number(const char *obj, float val) {
    for (auto &e : NUMBERS)
        if (strcmp(e.obj, obj) == 0) { e.set(val); mqtt_publish_all_states(); return true; }
    return false;
}
bool web_press_button(const char *obj) {
    for (auto &e : BUTTONS)
        if (strcmp(e.obj, obj) == 0) { e.press(); return true; }
    return false;
}
