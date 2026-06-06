// ================================================================
//  mqtt_net.h  —  esp-mqtt client, Home Assistant MQTT Discovery,
//  state publishing and command routing.
// ================================================================
#pragma once

#include <stddef.h>   // size_t

void mqtt_net_start(void);          // configure + start the client
void mqtt_publish_all_states(void); // publish every entity's current state

// ---- Web API (implemented in mqtt_net.cpp, used by ota.cpp web server) ----
int  web_build_state_json(char *buf, size_t n);   // full state as JSON
bool web_set_switch(const char *obj, bool on);
bool web_set_number(const char *obj, float val);
bool web_press_button(const char *obj);
