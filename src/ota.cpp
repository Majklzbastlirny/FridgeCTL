#include "ota.h"
#include "config.h"
#include "secrets.h"
#include "app_state.h"
#include "gpio_io.h"
#include "mqtt_net.h"
#include "diag.h"

#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";
static char s_expected_auth[128];

// ================================================================
//  Two-stage web UI
//    /            read-only dashboard (no password)
//    /api/state   JSON state (no password) — polled by both pages
//    /control     control surface + OTA   (Basic-Auth)
//    /api/cmd     POST control commands    (Basic-Auth)
//    /update      POST firmware image      (Basic-Auth)
// ================================================================

// ---- Read-only dashboard (public) ------------------------------
static const char *PAGE_VIEW =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>FridgeCTL</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#0f1115;color:#e6e6e6}"
"h1{font-size:1.2rem;margin:0;padding:14px 16px;background:#171a21;display:flex;justify-content:space-between;align-items:center}"
"a.btn{font-size:.8rem;background:#2a6;color:#fff;text-decoration:none;padding:6px 10px;border-radius:6px}"
".alarm{padding:10px 16px;font-weight:600}"
".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px;padding:12px}"
".card{background:#171a21;border-radius:8px;padding:10px}"
".card .n{font-size:.72rem;color:#9aa;text-transform:uppercase;letter-spacing:.03em}"
".card .v{font-size:1.25rem;margin-top:4px}"
".sec{padding:4px 16px;color:#9aa;font-size:.8rem;text-transform:uppercase;margin-top:8px}"
".pill{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px;vertical-align:middle}"
".on{background:#2a6}.off{background:#444}.warn{background:#e84}"
"</style></head><body>"
"<h1>FridgeCTL <a class=btn href=/control>Controls &rarr;</a></h1>"
"<div class=alarm id=alarm>&nbsp;</div>"
"<div class=sec>Temperatures &amp; metrics</div><div class=grid id=sensors></div>"
"<div class=sec>Status</div><div class=grid id=binary></div>"
"<div class=sec>System</div><div class=grid id=text></div>"
"<script>"
"async function tick(){try{let r=await fetch('/api/state');let d=await r.json();"
"let a=d.text.alarm_state?d.text.alarm_state.v:'';let ae=document.getElementById('alarm');"
"ae.textContent='Alarm: '+a;ae.style.background=(a=='OK'||a=='SYSTEM OFF')?'#171a21':"
"(a=='INITIAL COOLDOWN'||a=='DEFROST'||a=='COMPRESSOR COOLDOWN')?'#36506b':'#7a2d2d';"
"let s='';for(let k in d.sensors){let e=d.sensors[k];"
"s+='<div class=card><div class=n>'+e.n+'</div><div class=v>'+(e.v===null?'&mdash;':e.v+' '+e.u)+'</div></div>';}"
"document.getElementById('sensors').innerHTML=s;"
"let b='';for(let k in d.binary){let e=d.binary[k];"
"b+='<div class=card><span class=\"pill '+(e.v?'warn':'off')+'\"></span>'+e.n+'</div>';}"
"document.getElementById('binary').innerHTML=b;"
"let t='';for(let k in d.text){let e=d.text[k];"
"t+='<div class=card><div class=n>'+e.n+'</div><div class=v style=font-size:.95rem>'+e.v+'</div></div>';}"
"document.getElementById('text').innerHTML=t;"
"}catch(e){}}tick();setInterval(tick,3000);"
"</script></body></html>";

// ---- Control page (password) -----------------------------------
static const char *PAGE_CTRL =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>FridgeCTL Controls</title><style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#0f1115;color:#e6e6e6}"
"h1{font-size:1.2rem;margin:0;padding:14px 16px;background:#171a21;display:flex;justify-content:space-between;align-items:center}"
"a.btn{font-size:.8rem;background:#456;color:#fff;text-decoration:none;padding:6px 10px;border-radius:6px}"
".sec{padding:6px 16px;color:#9aa;font-size:.8rem;text-transform:uppercase;margin-top:8px}"
".row{display:flex;justify-content:space-between;align-items:center;padding:8px 16px;border-bottom:1px solid #222}"
"button{background:#2a6;color:#fff;border:0;padding:8px 12px;border-radius:6px;font-size:.9rem}"
"button.sw{min-width:64px}button.off{background:#444}button.act{background:#a63}"
"input{width:90px;background:#222;color:#fff;border:1px solid #444;border-radius:6px;padding:6px}"
"#ota{margin:12px 16px;padding:12px;background:#171a21;border-radius:8px}"
"#s{padding:0 16px;color:#9aa;font-size:.85rem}"
"</style></head><body>"
"<h1>FridgeCTL Controls <a class=btn href=/>&larr; View</a></h1>"
"<div id=s>loading&hellip;</div>"
"<div class=sec>Switches</div><div id=switches></div>"
"<div class=sec>Setpoints &amp; config</div><div id=numbers></div>"
"<div class=sec>Actions</div><div id=buttons></div>"
"<div class=sec>Firmware</div>"
"<div id=ota><input type=file id=f accept='.bin'> <button onclick=up()>Upload &amp; reboot</button>"
"<div id=ostat></div></div>"
"<script>"
"async function load(){let d=await(await fetch('/api/state')).json();"
"let sw='';for(let k in d.switches){let e=d.switches[k];"
"sw+='<div class=row><span>'+e.n+'</span><button class=\"sw '+(e.v?'':'off')+'\" onclick=\"sset(\\''+k+'\\','+(!e.v)+')\">'+(e.v?'ON':'OFF')+'</button></div>';}"
"document.getElementById('switches').innerHTML=sw;"
"let nm='';for(let k in d.numbers){let e=d.numbers[k];"
"nm+='<div class=row><span>'+e.n+' ('+e.u+')</span><span><input type=number id=\"n_'+k+'\" value='+e.v+' min='+e.min+' max='+e.max+' step='+e.step+'> <button onclick=\"nset(\\''+k+'\\')\">Set</button></span></div>';}"
"document.getElementById('numbers').innerHTML=nm;"
"let bt='';for(let k in d.buttons){bt+='<div class=row><span>'+d.buttons[k]+'</span><button class=act onclick=\"bpress(\\''+k+'\\')\">Run</button></div>';}"
"document.getElementById('buttons').innerHTML=bt;"
"document.getElementById('s').textContent='';}"
"async function post(o){await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});setTimeout(load,300);}"
"function sset(k,v){post({type:'switch',obj:k,val:v});}"
"function nset(k){post({type:'number',obj:k,val:parseFloat(document.getElementById('n_'+k).value)});}"
"function bpress(k){if(k=='restart'&&!confirm('Restart device?'))return;post({type:'button',obj:k});}"
"function reboot(o){let n=8;let iv=setInterval(()=>{"
"o.textContent='Firmware received \\u2014 device rebooting, reconnecting in '+n+'s\\u2026';"
"if(--n<0){clearInterval(iv);location.reload();}},1000);}"
"function up(){let f=document.getElementById('f').files[0];if(!f){alert('pick a .bin');return;}"
"let o=document.getElementById('ostat');let x=new XMLHttpRequest();x.open('POST','/update');"
"x.upload.onprogress=function(e){if(e.lengthComputable){let p=Math.round(e.loaded/e.total*100);"
"o.textContent='Uploading '+p+'% ('+Math.round(e.loaded/1024)+'/'+Math.round(e.total/1024)+' KB)\\u2026';"
"if(p>=100)o.textContent='Upload complete \\u2014 writing & verifying firmware (keep this tab open)\\u2026';}"
"else o.textContent='Uploading\\u2026';};"
"x.onload=function(){o.textContent=x.responseText;if(x.responseText.indexOf('received')>=0)reboot(o);};"
"x.onerror=function(){o.textContent='error: upload failed';};x.send(f);}"
"load();setInterval(load,5000);"
"</script></body></html>";

// ---- Basic-Auth ------------------------------------------------
static bool check_auth(httpd_req_t *req) {
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    return strcmp(hdr, s_expected_auth) == 0;
}
static esp_err_t deny(httpd_req_t *req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"FridgeCTL\"");
    httpd_resp_sendstr(req, "Authentication required");
    return ESP_OK;
}

// ---- Public endpoints ------------------------------------------
static esp_err_t view_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, PAGE_VIEW);
}

static esp_err_t state_get(httpd_req_t *req) {
    static char json[8192];           // static: keep it off the small task stack
    int len = web_build_state_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

// ---- Protected endpoints ---------------------------------------
static esp_err_t ctrl_get(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, PAGE_CTRL);
}

// Extract a JSON string field value (very small, fixed-format parser for the
// commands our own page sends; not a general JSON parser).
static bool json_field(const char *body, const char *key, char *out, size_t n) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == '"') {
        p++;
        size_t i = 0;
        while (*p && *p != '"' && i < n - 1) out[i++] = *p++;
        out[i] = 0;
    } else {
        size_t i = 0;
        while (*p && *p != ',' && *p != '}' && i < n - 1) out[i++] = *p++;
        out[i] = 0;
    }
    return true;
}

static esp_err_t cmd_post(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);
    char body[160];
    int n = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, n);
    if (got <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[got] = 0;

    char type[12], obj[32], val[24];
    if (!json_field(body, "type", type, sizeof(type)) ||
        !json_field(body, "obj",  obj,  sizeof(obj))) {
        httpd_resp_sendstr(req, "bad request"); return ESP_OK;
    }
    bool ok = false;
    if (strcmp(type, "switch") == 0 && json_field(body, "val", val, sizeof(val))) {
        ok = web_set_switch(obj, strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
    } else if (strcmp(type, "number") == 0 && json_field(body, "val", val, sizeof(val))) {
        ok = web_set_number(obj, strtof(val, nullptr));
    } else if (strcmp(type, "button") == 0) {
        ok = web_press_button(obj);
    }
    httpd_resp_sendstr(req, ok ? "ok" : "unknown command");
    return ESP_OK;
}

// ---- OTA upload ------------------------------------------------
// Mark the update as failed: latches the "FIRMWARE UPDATE FAILED" alarm,
// clears the in-progress flag, and reports the reason to the client.
static esp_err_t ota_fail(httpd_req_t *req, const char *msg) {
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    { StateGuard l; g.ota_in_progress = false; g.ota_failed = true; }
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

// Settle time after the fan relay opens, before we reboot — lets the
// inductive turn-off transient fully decay so it can't coincide with the
// watchdog-driven reset inside esp_restart().
#define OTA_FAN_SETTLE_MS 3000

static esp_err_t update_post(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);

    // --- Phase 1: stop the compressor only. Keep the FAN RUNNING. ---
    // The fan's inductive turn-off transient must NOT happen during the flash
    // write (cache disabled, CPU stalled = the fragile window that has crashed
    // the chip mid-OTA). So we leave the fan on through all flash activity and
    // only drop it afterwards. ota_in_progress also freezes the control loop's
    // relay handling (see control_tick) so it can't change relays under us.
    ESP_LOGW(TAG, "OTA starting - compressor off, fan kept on during flash");
    diag_event(DIAG_OTA);
    {
        StateGuard l;
        g.ota_in_progress = true;   // -> "FIRMWARE UPDATE"; freezes control loop
        g.ota_failed = false;
        relay_comp(false); g.comp_on = false; g.comp_stop_ms = millis();
        relay_k2(false);   g.k2_on = false;
        // fan deliberately left as-is
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (!part) return ota_fail(req, "no OTA partition");

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK)
        return ota_fail(req, "esp_ota_begin failed");

    // --- Phase 2: receive + write the image (flash busy here) ---
    char buf[1024];
    int remaining = req->content_len;
    int received;
    while (remaining > 0) {
        received = httpd_req_recv(req, buf, sizeof(buf) < (size_t)remaining ? sizeof(buf) : remaining);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
            esp_ota_abort(handle);
            return ota_fail(req, "recv error");
        }
        if (esp_ota_write(handle, buf, received) != ESP_OK) {
            esp_ota_abort(handle);
            return ota_fail(req, "ota_write failed");
        }
        remaining -= received;
    }

    if (esp_ota_end(handle) != ESP_OK)
        return ota_fail(req, "ota_end failed (bad image?)");
    if (esp_ota_set_boot_partition(part) != ESP_OK)
        return ota_fail(req, "set_boot_partition failed");

    // --- Phase 3: flash is done. NOW it's safe to drop the fan, let the
    // transient settle, then reboot. Reply first so the browser shows the
    // countdown message (the connection closes when we restart). ---
    ESP_LOGW(TAG, "OTA image OK - fan off, settling %d ms, then reboot",
             OTA_FAN_SETTLE_MS);
    {
        StateGuard l;
        relay_fan(false); g.fan_on = false;
    }
    char msg[96];
    snprintf(msg, sizeof(msg),
             "Firmware received. Fan off; restarting in %d s into new firmware.",
             OTA_FAN_SETTLE_MS / 1000);
    httpd_resp_sendstr(req, msg);

    vTaskDelay(pdMS_TO_TICKS(OTA_FAN_SETTLE_MS));   // fan transient decays here
    esp_restart();
    return ESP_OK;
}

void ota_start(void) {
    // Pre-compute the expected "Basic base64(user:pass)" header.
    char creds[96];
    int cl = snprintf(creds, sizeof(creds), "%s:%s", OTA_USERNAME, OTA_PASSWORD);
    unsigned char b64[128];
    size_t olen = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &olen, (unsigned char *)creds, cl);
    snprintf(s_expected_auth, sizeof(s_expected_auth), "Basic %.*s", (int)olen, b64);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    config.max_uri_handlers = 8;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        return;
    }
    httpd_uri_t u_view  = { "/",          HTTP_GET,  view_get,   nullptr };
    httpd_uri_t u_state = { "/api/state", HTTP_GET,  state_get,  nullptr };
    httpd_uri_t u_ctrl  = { "/control",   HTTP_GET,  ctrl_get,   nullptr };
    httpd_uri_t u_cmd   = { "/api/cmd",   HTTP_POST, cmd_post,   nullptr };
    httpd_uri_t u_upd   = { "/update",    HTTP_POST, update_post, nullptr };
    httpd_register_uri_handler(server, &u_view);
    httpd_register_uri_handler(server, &u_state);
    httpd_register_uri_handler(server, &u_ctrl);
    httpd_register_uri_handler(server, &u_cmd);
    httpd_register_uri_handler(server, &u_upd);

    ESP_LOGI(TAG, "web server ready: / (view), /control (auth), /update (auth)");
}

// ================================================================
//  Rollback health check
//
//  With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, a freshly-OTA'd image
//  boots in ESP_OTA_IMG_PENDING_VERIFY. If we never confirm it, the
//  bootloader reverts to the previous slot on the next reset. We only
//  confirm once the firmware proves itself healthy.
//
//  Health = the safety-critical control loop is genuinely running:
//    - it has finished its boot sequence (g.boot_done), and
//    - its heartbeat counter (g.control_beats) is advancing, i.e. the
//      5 s control tick is executing — not stuck/crashed/deadlocked.
//
//  WiFi/MQTT are deliberately NOT health criteria. This fridge is
//  designed to run fully offline; a router/broker outage must never
//  cause a good firmware to be rolled back. The control loop driving
//  the relays is what "working" means here.
//
//  A USB-flashed image is normally already marked valid; this task is a
//  no-op in that case (it only acts on a PENDING_VERIFY state).
// ================================================================
static bool image_pending_verify(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void rollback_check_task(void *arg) {
    if (!image_pending_verify()) {
        ESP_LOGI(TAG, "image already valid, no rollback check needed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "image PENDING_VERIFY - running health check before commit");

    // Wait for the control loop to be up and its heartbeat to advance.
    // Boot does an LED test + 30 s sensor warmup before boot_done flips and
    // the first tick runs, so allow generous time. We require the heartbeat
    // to move by >= 2 (i.e. at least one full control tick observed live),
    // proving the safety loop is actually executing, not just initialised.
    const int    give_up   = 90;     // ~90 s budget (covers the warmup)
    uint32_t     beats0;
    { StateGuard l; beats0 = g.control_beats; }

    for (int i = 0; i < give_up; i++) {
        bool done; uint32_t beats;
        { StateGuard l; done = g.boot_done; beats = g.control_beats; }
        if (done && beats >= beats0 + 2) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "health check PASSED - control loop alive, image committed");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Never got healthy: roll back to the previous image now.
    ESP_LOGE(TAG, "health check FAILED - rolling back to previous firmware");
    esp_ota_mark_app_invalid_rollback_and_reboot();   // does not return
    vTaskDelete(NULL);
}

void ota_health_check_start(void) {
    xTaskCreate(rollback_check_task, "ota_verify", 4096, nullptr, 3, nullptr);
}
