#pragma once
// ================================================================
//  net_cfg  —  WiFi + MQTT connection settings.
//
//  Firmware defaults come from secrets.h. Anything saved from the web
//  control page (/api/net) is stored in NVS namespace "net" and WINS over
//  secrets.h until "Use firmware defaults" erases it. Loaded once at boot
//  (before wifi_net_start / mqtt_net_start) and immutable afterwards: a
//  change is applied by saving and rebooting.
// ================================================================
#include <stdbool.h>

struct NetCfg {
    char ssid[33];        // 802.11 max 32 bytes
    char pass[65];        // WPA2 PSK: 8..63 chars, or 64 hex; "" = open network
    char mqtt_uri[128];
    char mqtt_user[64];
    char mqtt_pass[64];
    bool from_nvs;        // true = saved via web UI, false = secrets.h defaults
};

void          net_cfg_load(void);                 // call after nvs_flash_init
const NetCfg &net_cfg(void);                      // the settings in use this boot
bool          net_cfg_save(const NetCfg &c);      // persist (takes effect on reboot)
bool          net_cfg_reset(void);                // erase -> secrets.h on next boot
