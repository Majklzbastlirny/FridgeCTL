// ================================================================
//  nvs_store.h  —  persistence of the restore_value:yes fields.
//
//  Mirrors ESPHome's preferences.flash_write_interval (5 min): the
//  control task marks state dirty; a low-priority committer flushes
//  to NVS at most every 5 minutes to spare the flash.
// ================================================================
#pragma once

void nvs_store_init(void);     // open NVS, load values into g
void nvs_store_load(void);     // load persisted fields into g
void nvs_store_save(void);     // write current persisted fields to NVS (blocking)
void nvs_store_mark_dirty(void);   // request a deferred save
void nvs_store_task_start(void);   // spawn the periodic committer task
