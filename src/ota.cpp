#include "ota.h"
#include "config.h"
#include "secrets.h"
#include "app_state.h"
#include "gpio_io.h"
#include "mqtt_net.h"
#include "diag.h"
#include "sensors.h"
#include "nvs_store.h"
#include "logbuf.h"
#include "net_cfg.h"

#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "web";
static char s_expected_auth[128];

// ================================================================
//  Two-stage web UI
//    /            read-only dashboard (no password)
//    /api/state   JSON state (no password) — polled by both pages
//    /control     control surface + OTA   (Basic-Auth)
//    /api/cmd     POST control commands    (Basic-Auth)
//    /api/onewire GET  1-Wire bus inventory (Basic-Auth)
//    /api/logs    GET  raw log tail        (Basic-Auth)
//    /api/net     GET/POST WiFi + MQTT settings (Basic-Auth)
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
"button.mini{font-size:.72rem;padding:4px 8px;background:#456;margin-left:6px}"
".note{padding:4px 16px;color:#778;font-size:.75rem}"
".owb{padding:10px 16px 2px;color:#9aa;font-size:.78rem}"
".ow{padding:6px 16px;border-bottom:1px solid #222;font-size:.85rem}"
".rom{font-family:ui-monospace,Consolas,monospace;font-size:.8rem;color:#cde}"
".badge{font-size:.68rem;background:#345;padding:2px 6px;border-radius:4px;margin-left:8px}"
".badge.a{background:#2a6}.badge.f{background:#36c}"
"#log{background:#0b0d11;font:11px/1.45 ui-monospace,Consolas,monospace;"
"padding:8px 12px;margin:0 16px 16px;height:340px;overflow:auto;white-space:pre-wrap;border-radius:8px}"
"#log .w{color:#e84}#log .e{color:#f66}"
"#net input{width:min(240px,55vw)}"
"</style></head><body>"
"<h1>FridgeCTL Controls <a class=btn href=/>&larr; View</a></h1>"
"<div id=s>loading&hellip;</div>"
"<div class=sec>Switches</div><div id=switches></div>"
"<div class=sec>Setpoints &amp; config</div><div id=numbers></div>"
"<div class=sec>Actions</div><div id=buttons></div>"
"<div class=sec>1-Wire sensors <button class=mini onclick=owscan()>Rescan</button></div>"
"<div id=ow></div>"
"<div class=note>Calibration offsets follow the role, not the probe &mdash; re-check them after reassigning.</div>"
"<div class=sec>Logs <button class=mini onclick=lcopy()>Copy</button>"
"<button class=mini onclick=lclear()>Clear</button>"
"<label style='margin-left:10px;text-transform:none'><input type=checkbox id=lf checked> follow</label>"
"<span id=lstat></span></div>"
"<pre id=log></pre>"
// ---- Network settings. Own container, loaded once (not by load()'s 5 s
// re-render) so typing into the fields is never overwritten.
"<div class=sec>Network</div>"
"<div id=net>"
"<div class=row><span>WiFi SSID</span><input id=ns maxlength=32 autocomplete=off></div>"
"<div class=row><span>WiFi password</span><input id=np type=password maxlength=64 autocomplete=new-password></div>"
"<div class=row><span>MQTT broker URI</span><input id=nu maxlength=127 placeholder='mqtt://host:1883'></div>"
"<div class=row><span>MQTT username</span><input id=nm maxlength=63 autocomplete=off></div>"
"<div class=row><span>MQTT password</span><input id=nq type=password maxlength=63 autocomplete=new-password></div>"
"<div class=note id=ni></div>"
"<div class=row><button onclick=nsave()>Save &amp; reboot</button>"
"<button class=act onclick=nreset()>Use firmware defaults</button></div>"
"<div class=note id=nst></div></div>"
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
// ---- 1-Wire inventory. Own container + own poll: load() replaces the
// switches/numbers/buttons markup wholesale and would eat a click target.
// Buttons carry data- attributes and a single delegated handler, so no
// quoting gymnastics inside generated onclick strings.
"let owprev='';"
"async function owload(){try{"
"let t=await(await fetch('/api/onewire')).text();"
"if(t==owprev)return;owprev=t;let d=JSON.parse(t);"
"let h=d.scanning?'<div class=note>scanning\\u2026</div>':'';"
"for(let b of d.buses){"
"h+='<div class=owb>'+b.n+' (GPIO'+b.gpio+') \\u2014 '+(d.scanned?b.dev.length+' device(s)':'not scanned yet')+'</div>';"
"for(let x of b.dev){"
"h+='<div class=ow><span class=rom>'+x.rom+'</span> '+(x.t===null?'&mdash;':x.t.toFixed(2)+' \\u00b0C');"
"if(x.role)h+='<span class=\"badge '+(x.role=='ambient'?'a':x.role=='freezer'?'f':'')+'\">'+x.role+'</span>';"
"if(!b.fixed)h+='<span style=float:right><button class=mini data-rom='+x.rom+' data-role=ambient>Ambient</button>'"
"+'<button class=mini data-rom='+x.rom+' data-role=freezer>Freezer</button></span>';"
"h+='</div>';}"
"if(d.scanned&&!b.dev.length)h+='<div class=ow style=color:#e84>no devices responded</div>';}"
"document.getElementById('ow').innerHTML=h;}catch(e){}}"
"document.getElementById('ow').onclick=async function(e){"
"let t=e.target;if(!t.dataset||!t.dataset.role)return;"
"if(!confirm('Set '+t.dataset.rom+' as '+t.dataset.role+'?'))return;"
"let r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({type:'rom',obj:t.dataset.role,val:t.dataset.rom})});"
"let m=await r.text();if(m!='ok')alert(m);owprev='';owload();};"
"function owscan(){owprev='';post({type:'button',obj:'ow_scan'});"
"setTimeout(owload,1500);setTimeout(owload,3500);}"
"owload();setInterval(owload,5000);"
// ---- Raw log tail. ?since= means each poll carries only new lines, so the
// 2 s cadence costs almost nothing. New lines are appended as nodes rather
// than re-rendering, and auto-scroll backs off while you read scrolled up.
"let lseq=0,llines=[];"
"async function lload(){try{"
"let t=await(await fetch('/api/logs?since='+lseq)).text();"
"let el=document.getElementById('log');"
"let atB=el.scrollHeight-el.scrollTop-el.clientHeight<40;let add=0;"
"for(let ln of t.split('\\n')){"
"if(ln.indexOf('#seq ')==0){lseq=parseInt(ln.slice(5))||lseq;continue;}"
"if(!ln)continue;llines.push(ln);"
"let s=document.createElement('span');"
"s.className=ln[0]=='W'?'w':ln[0]=='E'?'e':'';"
"s.textContent=ln+'\\n';el.appendChild(s);add++;}"
"if(!add)return;"
"while(llines.length>2000)llines.shift();"
"while(el.childNodes.length>2000)el.removeChild(el.firstChild);"
"if(document.getElementById('lf').checked||atB)el.scrollTop=el.scrollHeight;"
"}catch(e){}}"
"function lclear(){llines=[];document.getElementById('log').innerHTML='';}"
// Plain http:// is not a secure context, so navigator.clipboard is usually
// undefined here — the textarea + execCommand path is the one that fires.
"function lcopy(){let s=document.getElementById('lstat');let txt=llines.join('\\n');"
"let ok=function(){s.textContent=' copied';setTimeout(function(){s.textContent='';},1500);};"
"if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(txt).then(ok);return;}"
"let ta=document.createElement('textarea');ta.value=txt;ta.style.position='fixed';ta.style.opacity=0;"
"document.body.appendChild(ta);ta.select();"
"try{if(document.execCommand('copy'))ok();else s.textContent=' copy failed';}"
"catch(e){s.textContent=' copy failed';}"
"document.body.removeChild(ta);}"
"lload();setInterval(lload,2000);"
// ---- Network settings. Passwords are never sent to the browser: the
// placeholder only says whether one is set, and an empty field keeps it.
"function $(i){return document.getElementById(i);}"
"async function nload(){try{let d=await(await fetch('/api/net')).json();"
"$('ns').value=d.ssid;$('nu').value=d.uri;$('nm').value=d.user;$('np').value='';$('nq').value='';"
"$('np').placeholder=d.pass_set?'(unchanged)':'(none)';$('nq').placeholder=d.mpass_set?'(unchanged)':'(none)';"
"$('ni').textContent='In use: '+(d.saved?'settings saved here':'firmware defaults (secrets.h)')"
"+' \\u2014 IP '+d.ip+', WiFi '+(d.wifi?'connected':'down')+', MQTT '+(d.mqtt?'connected':'down')"
"+'. Leave a password empty to keep it. If WiFi fails, connect to hotspot '+d.ap+' and open http://192.168.4.1/control';"
"}catch(e){}}"
"async function nsend(o,q){if(!confirm(q))return;$('nst').textContent='sending\\u2026';"
"try{let r=await fetch('/api/net',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});"
"$('nst').textContent=await r.text();}catch(e){$('nst').textContent='device is rebooting\\u2026';}}"
"function nsave(){let o={ssid:$('ns').value,pass:$('np').value,uri:$('nu').value,user:$('nm').value,mpass:$('nq').value};"
"nsend(o,'Save and reboot? The device will join '+o.ssid+' and may get a new IP address. '"
"+'If it cannot connect, use the fallback hotspot at http://192.168.4.1/control.');}"
"function nreset(){nsend({reset:true},'Erase the saved network settings and reboot with the firmware defaults (secrets.h)?');}"
"nload();"
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

// Point one of the two shared-bus roles at a specific ROM. Returns nullptr on
// success, else a short message for the user.
static const char *assign_rom(const char *role, const char *hex) {
    bool is_amb = strcmp(role, "ambient") == 0;
    if (!is_amb && strcmp(role, "freezer") != 0) return "unknown role";

    uint64_t rom = strtoull(hex, nullptr, 16);
    if (rom == 0) return "bad ROM code";

    // Only accept a device the last scan actually saw on the shared bus, so a
    // stale page or a typo can never point a role at nothing.
    OwFound found[OW_SCAN_MAX];
    int n = sensors_scan_results(found, OW_SCAN_MAX, nullptr);
    bool on_shared = false;
    for (int i = 0; i < n; i++) {
        const OwBusInfo *bi = sensors_bus_info(found[i].bus);
        if (found[i].rom == rom && bi && bi->fixed_role[0] == 0) on_shared = true;
    }
    if (!on_shared) return "that ROM is not on the shared bus - rescan first";

    {
        StateGuard l;
        uint64_t cur_amb = g.addr_ambient ? g.addr_ambient : ADDR_AMBIENT;
        uint64_t cur_frz = g.addr_freezer ? g.addr_freezer : ADDR_FREEZER;
        // Claiming the probe the other role currently owns hands that role
        // ours, so the two can be swapped in a single click.
        if (is_amb) {
            if (rom == cur_frz) g.addr_freezer = cur_amb;
            g.addr_ambient = rom;
        } else {
            if (rom == cur_amb) g.addr_ambient = cur_frz;
            g.addr_freezer = rom;
        }
    }
    // Rare and deliberate, so commit right now instead of waiting up to five
    // minutes for the deferred committer — a reboot must not lose this.
    nvs_store_save();
    sensors_roles_changed();
    ESP_LOGW(TAG, "1-Wire role '%s' assigned to %s", role, hex);
    return nullptr;
}

static esp_err_t cmd_post(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);
    char body[224];
    int n = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, n);
    if (got <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    body[got] = 0;

    char type[12], obj[32], val[32];
    if (!json_field(body, "type", type, sizeof(type)) ||
        !json_field(body, "obj",  obj,  sizeof(obj))) {
        httpd_resp_sendstr(req, "bad request"); return ESP_OK;
    }
    bool ok = false;
    const char *err = "unknown command";
    if (strcmp(type, "switch") == 0 && json_field(body, "val", val, sizeof(val))) {
        ok = web_set_switch(obj, strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
    } else if (strcmp(type, "number") == 0 && json_field(body, "val", val, sizeof(val))) {
        ok = web_set_number(obj, strtof(val, nullptr));
    } else if (strcmp(type, "button") == 0) {
        ok = web_press_button(obj);
    } else if (strcmp(type, "rom") == 0 && json_field(body, "val", val, sizeof(val))) {
        err = assign_rom(obj, val);          // obj = "ambient" | "freezer"
        ok  = (err == nullptr);
    }
    httpd_resp_sendstr(req, ok ? "ok" : err);
    return ESP_OK;
}

// ---- 1-Wire bus inventory --------------------------------------
// MSB-first hex, so a ROM reads back character-for-character identical to the
// ADDR_AMBIENT / ADDR_FREEZER literals in config.h (family code 0x28 is the
// last two characters). Split into two 32-bit halves to stay clear of any
// "ll" length-modifier trouble in the reduced newlib printf.
static void rom_hex(char *out, uint64_t rom) {
    sprintf(out, "%08lX%08lX", (unsigned long)(rom >> 32),
                               (unsigned long)(rom & 0xFFFFFFFFUL));
}

static esp_err_t onewire_get(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);

    OwFound  found[OW_SCAN_MAX];
    uint32_t age = 0;
    int      n   = sensors_scan_results(found, OW_SCAN_MAX, &age);

    uint64_t cur_amb, cur_frz;
    {
        StateGuard l;
        cur_amb = g.addr_ambient ? g.addr_ambient : ADDR_AMBIENT;
        cur_frz = g.addr_freezer ? g.addr_freezer : ADDR_FREEZER;
    }

    static char buf[1536];       // static: keep it off the HTTP task stack
    int p = 0;
    // Bounded append — never let the remaining size go negative (snprintf
    // returns what it *would* have written, which would wrap size_t).
    #define J(...) do { \
        if (p < (int)sizeof(buf)) p += snprintf(buf + p, sizeof(buf) - p, __VA_ARGS__); \
        if (p > (int)sizeof(buf)) p = (int)sizeof(buf); \
    } while (0)

    J("{\"scanning\":%s,\"scanned\":%s,\"age\":%lu,\"buses\":[",
      sensors_scan_pending() ? "true" : "false",
      sensors_scan_done()    ? "true" : "false",
      (unsigned long)age);

    for (int b = 0; b < OW_BUS_COUNT; b++) {
        const OwBusInfo *bi = sensors_bus_info(b);
        J("%s{\"n\":\"%s\",\"gpio\":%u,\"fixed\":\"%s\",\"dev\":[",
          b ? "," : "", bi->name, (unsigned)bi->gpio, bi->fixed_role);
        bool first = true;
        for (int i = 0; i < n; i++) {
            if (found[i].bus != b) continue;
            char hex[17];
            rom_hex(hex, found[i].rom);
            const char *role = "";
            if      (found[i].rom == cur_amb) role = "ambient";
            else if (found[i].rom == cur_frz) role = "freezer";
            else if (bi->fixed_role[0])       role = bi->fixed_role;
            J("%s{\"rom\":\"%s\",\"role\":\"%s\",\"t\":", first ? "" : ",", hex, role);
            if (isnan(found[i].temp)) J("null}");
            else                      J("%.2f}", found[i].temp);
            first = false;
        }
        J("]}");
    }
    J("]}");
    #undef J

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, p);
}

// ---- Raw log tail ----------------------------------------------
// Streamed in chunks so we don't need a second multi-KB static buffer beside
// the /api/state one. `?since=N` makes each poll carry only what is new. The
// trailing "#seq N" line tells the client what to ask for next time — it is
// written last so lines that arrive mid-response are not skipped.
// Auth-gated: log lines contain the SSID, IP and OTA detail.
static esp_err_t logs_get(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);

    uint32_t since = 0;
    char q[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK)
            since = (uint32_t)strtoul(v, nullptr, 10);
    }

    httpd_resp_set_type(req, "text/plain");

    char     chunk[512];
    uint32_t seq = since;
    for (;;) {
        uint32_t next = seq;
        int len = logbuf_dump(chunk, sizeof(chunk), seq, &next);
        if (len > 0) httpd_resp_send_chunk(req, chunk, len);
        if (next == seq) break;          // caught up
        seq = next;
    }
    int hdr = snprintf(chunk, sizeof(chunk), "#seq %lu\n", (unsigned long)seq);
    httpd_resp_send_chunk(req, chunk, hdr);
    return httpd_resp_send_chunk(req, nullptr, 0);
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

// ================================================================
//  Network settings (/api/net) — WiFi + MQTT, see net_cfg.h.
//  Saved settings win over secrets.h until "Use firmware defaults".
//  The fallback hotspot is always on (APSTA), so a wrong SSID/password
//  never strands the device: http://192.168.4.1/control fixes it.
// ================================================================

// Reboot without the fan's inductive turn-off transient coinciding with the
// reset (same reason as OTA phase 3). Callers write NVS BEFORE this, so no
// relay changes during that flash write.
static void safe_restart(const char *why) {
    ESP_LOGW(TAG, "%s - relays off, settling %d ms, then reboot", why, OTA_FAN_SETTLE_MS);
    {
        StateGuard l;
        g.restart_pending = true;          // control loop stops touching relays
        relay_comp(false);
        if (g.comp_on) { g.comp_on = false; g.comp_stop_ms = millis(); }
        relay_k2(false);  g.k2_on = false;
        relay_fan(false); g.fan_on = false;
    }
    vTaskDelay(pdMS_TO_TICKS(OTA_FAN_SETTLE_MS));
    esp_restart();
}

// Copy a JSON string value into `out`, undoing the escapes JSON.stringify
// emits (\" \\ \/ \b \f \n \r \t \uXXXX). Returns false if the key is
// missing, malformed or the value doesn't fit. It never truncates: a silently
// shortened password would leave the device on wrong credentials.
static bool json_str_field(const char *body, const char *key, char *out, size_t n) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p++ != '"') return false;
    size_t i = 0;
    while (*p && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c == '\\') {
            if (!*p) return false;
            char e = *p++;
            if      (e == '"' || e == '\\' || e == '/') c = (unsigned char)e;
            else if (e == 'b') c = '\b';
            else if (e == 'f') c = '\f';
            else if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == 'u') {
                unsigned v = 0;
                for (int k = 0; k < 4; k++) {
                    char h = *p++;
                    v <<= 4;
                    if      (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                if (v >= 0x80) {             // UTF-8 encode (BMP)
                    char u[3]; size_t ul;
                    if (v < 0x800) { u[0] = (char)(0xC0 | (v >> 6)); u[1] = (char)(0x80 | (v & 0x3F)); ul = 2; }
                    else { u[0] = (char)(0xE0 | (v >> 12)); u[1] = (char)(0x80 | ((v >> 6) & 0x3F));
                           u[2] = (char)(0x80 | (v & 0x3F)); ul = 3; }
                    if (i + ul >= n) return false;
                    memcpy(out + i, u, ul); i += ul;
                    continue;
                }
                c = v;
            } else return false;
        }
        if (i + 1 >= n) return false;
        out[i++] = (char)c;
    }
    if (*p != '"') return false;             // unterminated string
    out[i] = 0;
    return true;
}

// Bounded JSON writer for net_get (never overflows; escapes " \ and control chars).
struct JOut {
    char *b; size_t n, p = 0;
    void raw(const char *s) { while (*s && p + 1 < n) b[p++] = *s++; b[p] = 0; }
    void str(const char *s) {
        raw("\"");
        for (; *s; s++) {
            unsigned char c = (unsigned char)*s;
            char t[8];
            if (c == '"' || c == '\\') { t[0] = '\\'; t[1] = (char)c; t[2] = 0; }
            else if (c < 0x20)        snprintf(t, sizeof(t), "\\u%04x", c);
            else                      { t[0] = (char)c; t[1] = 0; }
            raw(t);
        }
        raw("\"");
    }
};

static esp_err_t net_get(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);
    const NetCfg &c = net_cfg();
    char ip[16]; bool wifi, mqtt;
    { StateGuard l; strlcpy(ip, g.ip_addr, sizeof(ip)); wifi = g.wifi_connected; mqtt = g.mqtt_connected; }
    char buf[1024];
    JOut o{buf, sizeof(buf)};
    o.raw("{\"ssid\":");  o.str(c.ssid);
    o.raw(",\"uri\":");   o.str(c.mqtt_uri);
    o.raw(",\"user\":");  o.str(c.mqtt_user);
    o.raw(",\"ap\":");    o.str(AP_SSID);
    o.raw(",\"ip\":");    o.str(ip);
    o.raw(c.pass[0]      ? ",\"pass_set\":true"  : ",\"pass_set\":false");
    o.raw(c.mqtt_pass[0] ? ",\"mpass_set\":true" : ",\"mpass_set\":false");
    o.raw(c.from_nvs     ? ",\"saved\":true"     : ",\"saved\":false");
    o.raw(wifi           ? ",\"wifi\":true"      : ",\"wifi\":false");
    o.raw(mqtt           ? ",\"mqtt\":true}"     : ",\"mqtt\":false}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, o.p);
}

static bool is_hex(const char *s) {
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F')))
            return false;
    return true;
}

static esp_err_t net_post(httpd_req_t *req) {
    if (!check_auth(req)) return deny(req);
    char body[768];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_sendstr(req, "error: request too large");
        return ESP_OK;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
        got += r;
    }
    body[got] = 0;

    char flag[8];
    if (json_field(body, "reset", flag, sizeof(flag)) && strcmp(flag, "true") == 0) {
        if (!net_cfg_reset()) { httpd_resp_sendstr(req, "error: could not erase saved settings"); return ESP_OK; }
        httpd_resp_sendstr(req, "Saved network settings erased. Rebooting in 3 s with the firmware defaults (secrets.h).");
        safe_restart("network settings reset");
        return ESP_OK;
    }

    // Start from what's in use, so an empty password field keeps the current one.
    NetCfg c = net_cfg();
    char pass[sizeof(c.pass)], mpass[sizeof(c.mqtt_pass)];
    if (!json_str_field(body, "ssid",  c.ssid,      sizeof(c.ssid)) ||
        !json_str_field(body, "pass",  pass,        sizeof(pass)) ||
        !json_str_field(body, "uri",   c.mqtt_uri,  sizeof(c.mqtt_uri)) ||
        !json_str_field(body, "user",  c.mqtt_user, sizeof(c.mqtt_user)) ||
        !json_str_field(body, "mpass", mpass,       sizeof(mpass))) {
        httpd_resp_sendstr(req, "error: a field is missing or too long (SSID max 32 bytes, "
                                "WiFi password max 64, broker URI max 127, MQTT user/password max 63)");
        return ESP_OK;
    }
    size_t pl = strlen(pass);
    if (!c.ssid[0]) { httpd_resp_sendstr(req, "error: WiFi SSID is empty"); return ESP_OK; }
    if (pl && !(pl >= 8 && pl <= 63) && !(pl == 64 && is_hex(pass))) {
        httpd_resp_sendstr(req, "error: WiFi password must be 8-63 characters (or 64 hex digits)");
        return ESP_OK;
    }
    if (strncmp(c.mqtt_uri, "mqtt://", 7) != 0 || !c.mqtt_uri[7]) {
        httpd_resp_sendstr(req, "error: broker URI must look like mqtt://host:1883");
        return ESP_OK;
    }
    if (pl) strlcpy(c.pass, pass, sizeof(c.pass));
    if (mpass[0]) strlcpy(c.mqtt_pass, mpass, sizeof(c.mqtt_pass));
    c.from_nvs = true;

    if (!net_cfg_save(c)) { httpd_resp_sendstr(req, "error: could not save settings"); return ESP_OK; }
    char msg[160];
    snprintf(msg, sizeof(msg), "Saved. Rebooting in 3 s; the device will join '%s'.", c.ssid);
    httpd_resp_sendstr(req, msg);
    safe_restart("network settings saved");
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
    config.max_uri_handlers = 10;

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
    httpd_uri_t u_ow    = { "/api/onewire", HTTP_GET, onewire_get, nullptr };
    httpd_uri_t u_log   = { "/api/logs",  HTTP_GET,  logs_get,   nullptr };
    httpd_uri_t u_netg  = { "/api/net",   HTTP_GET,  net_get,    nullptr };
    httpd_uri_t u_netp  = { "/api/net",   HTTP_POST, net_post,   nullptr };
    httpd_register_uri_handler(server, &u_view);
    httpd_register_uri_handler(server, &u_state);
    httpd_register_uri_handler(server, &u_ctrl);
    httpd_register_uri_handler(server, &u_cmd);
    httpd_register_uri_handler(server, &u_upd);
    httpd_register_uri_handler(server, &u_ow);
    httpd_register_uri_handler(server, &u_log);
    httpd_register_uri_handler(server, &u_netg);
    httpd_register_uri_handler(server, &u_netp);

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
