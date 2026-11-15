#include "net_cfg.h"
#include "secrets.h"

#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "netcfg";
static const char *NS  = "net";
static NetCfg s_cfg;

// Read one string key; leaves `out` untouched if the key is missing or
// doesn't fit (so the secrets.h default stays). Returns true if read.
static bool get_str(nvs_handle_t h, const char *k, char *out, size_t n) {
    size_t len = 0;
    if (nvs_get_str(h, k, nullptr, &len) != ESP_OK || len > n) return false;
    return nvs_get_str(h, k, out, &len) == ESP_OK;
}

void net_cfg_load(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_cfg.ssid,      WIFI_SSID,     sizeof(s_cfg.ssid));
    strlcpy(s_cfg.pass,      WIFI_PASSWORD, sizeof(s_cfg.pass));
    strlcpy(s_cfg.mqtt_uri,  MQTT_URI,      sizeof(s_cfg.mqtt_uri));
#if defined(MQTT_USERNAME)
    strlcpy(s_cfg.mqtt_user, MQTT_USERNAME, sizeof(s_cfg.mqtt_user));
    strlcpy(s_cfg.mqtt_pass, MQTT_PASSWORD, sizeof(s_cfg.mqtt_pass));
#endif

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        bool any = false;
        any |= get_str(h, "ssid",  s_cfg.ssid,      sizeof(s_cfg.ssid));
        any |= get_str(h, "pass",  s_cfg.pass,      sizeof(s_cfg.pass));
        any |= get_str(h, "uri",   s_cfg.mqtt_uri,  sizeof(s_cfg.mqtt_uri));
        any |= get_str(h, "user",  s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user));
        any |= get_str(h, "mpass", s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass));
        s_cfg.from_nvs = any;
        nvs_close(h);
    }
    // SSID / URI are not secret; passwords are never logged.
    ESP_LOGI(TAG, "network settings from %s: SSID '%s', MQTT %s",
             s_cfg.from_nvs ? "web UI (NVS)" : "firmware defaults (secrets.h)",
             s_cfg.ssid, s_cfg.mqtt_uri);
}

const NetCfg &net_cfg(void) { return s_cfg; }

bool net_cfg_save(const NetCfg &c) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "ssid",  c.ssid)      == ESP_OK &&
              nvs_set_str(h, "pass",  c.pass)      == ESP_OK &&
              nvs_set_str(h, "uri",   c.mqtt_uri)  == ESP_OK &&
              nvs_set_str(h, "user",  c.mqtt_user) == ESP_OK &&
              nvs_set_str(h, "mpass", c.mqtt_pass) == ESP_OK &&
              nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGW(TAG, "network settings %s: SSID '%s', MQTT %s",
             ok ? "saved" : "SAVE FAILED", c.ssid, c.mqtt_uri);
    return ok;
}

bool net_cfg_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    ESP_LOGW(TAG, "network settings %s - firmware defaults on next boot",
             ok ? "erased" : "ERASE FAILED");
    return ok;
}
