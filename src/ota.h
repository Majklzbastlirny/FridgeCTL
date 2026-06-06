// ================================================================
//  ota.h  —  self-hosted HTTP OTA. Browse to http://<device-ip>/
//  (Basic-Auth) and upload a firmware.bin, or:
//     curl -u user:pass --data-binary @firmware.bin http://<ip>/update
// ================================================================
#pragma once

void ota_start(void);   // start the HTTP server (call once WiFi is up-ish)

// Start the rollback health-check task. With bootloader rollback enabled,
// a freshly-OTA'd image is only committed after it proves healthy; an
// unhealthy image is reverted to the previous slot. No-op for an image
// that is already marked valid (e.g. a normal USB flash).
void ota_health_check_start(void);
