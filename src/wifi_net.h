// ================================================================
//  wifi_net.h  —  WiFi station with an always-available fallback AP
//  (APSTA) so the device is always reachable for OTA recovery.
// ================================================================
#pragma once
#include <stdbool.h>

void wifi_net_start(void);     // init netif + event loop + connect
void wifi_update_status(void); // refresh RSSI/IP/SSID into g
