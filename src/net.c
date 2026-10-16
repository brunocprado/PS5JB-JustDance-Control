/*
    net.c - PS5JustDance
    Background network thread handling:
      - UDP port 9052 (JDM1 binary packets, Cemuhook DSUS packets, JSON packets)
      - Active Cemuhook DSUC subscription when dsu_server_ip is configured
      - HTTP port 8080 + Self-Signed HTTPS port 8443 serving the built-in
        Smartphone Web Controller UI (HTTPS is required by iOS Safari and
        Android Chrome to unlock DeviceMotionEvent IMU sensors!)

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "log.h"
#include "net.h"
#include "protocol.h"

static pthread_t g_net_thread;
static bool g_net_running = false;
static volatile bool g_net_stop = false;
static config_t g_cfg;

static pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static stub_controller_state_t g_controllers[SCE_MOVE_MAX_PLAYERS];
static uint64_t g_last_seen_ms[SCE_MOVE_MAX_PLAYERS];
static uint64_t g_total_packets = 0;
static bool g_dirty = true;

static uint64_t monotonic_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

static uint64_t monotonic_ms(void) {
    return monotonic_us() / 1000ull;
}

static void init_controller_defaults(void) {
    memset(g_controllers, 0, sizeof(g_controllers));
    memset(g_last_seen_ms, 0, sizeof(g_last_seen_ms));
    for (int i = 0; i < SCE_MOVE_MAX_PLAYERS; i++) {
        g_controllers[i].accel_bits[0] = IEEE754_ZERO_BITS;
        g_controllers[i].accel_bits[1] = IEEE754_ZERO_BITS;
        g_controllers[i].accel_bits[2] = IEEE754_ONE_BITS;
    }
}

static void apply_motion_sample(const motion_sample_t *s) {
    if (s->player_index < 0 || s->player_index >= SCE_MOVE_MAX_PLAYERS)
        return;

    uint64_t now_us = monotonic_us();
    uint64_t now_ms = now_us / 1000ull;

    pthread_mutex_lock(&g_state_mutex);
    stub_controller_state_t *c = &g_controllers[s->player_index];
    bool newly_connected = (c->connected == 0);
    c->connected = 1;
    c->seq++;
    c->accel_bits[0] = float_to_bits(s->accel_g[0]);
    c->accel_bits[1] = float_to_bits(s->accel_g[1]);
    c->accel_bits[2] = float_to_bits(s->accel_g[2]);
    c->gyro_bits[0]  = float_to_bits(s->gyro_rads[0]);
    c->gyro_bits[1]  = float_to_bits(s->gyro_rads[1]);
    c->gyro_bits[2]  = float_to_bits(s->gyro_rads[2]);
    c->buttons       = s->buttons;
    c->trigger       = s->trigger;
    c->pad_buttons   = s->pad_buttons;
    c->timestamp_us  = now_us;
    g_last_seen_ms[s->player_index] = now_ms;
    g_total_packets++;
    g_dirty = true;
    pthread_mutex_unlock(&g_state_mutex);

    if (newly_connected)
        LOG("virtual PS Move slot %d (Player %d) connected", s->player_index, s->player_index + 1);
}

/* ------------------------------------------------------------------ */
/* Embedded Smartphone Web Controller HTML                             */
/* ------------------------------------------------------------------ */

static const char k_phone_html[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Cache-Control: no-store\r\n"
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
    "<title>PS5 Just Dance Controller</title>"
    "<style>"
    "*{box-sizing:border-box;-webkit-tap-highlight-color:transparent;user-select:none;-webkit-user-select:none;}"
    "body{margin:0;padding:10px 12px 20px;background:radial-gradient(circle at top,#1f183d 0%,#090b12 80%);"
    "color:#fff;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;text-align:center;touch-action:manipulation;}"
    ".hdr{display:flex;align-items:center;justify-content:space-between;padding:4px 2px 8px;gap:8px;}"
    ".title{font-size:20px;font-weight:900;letter-spacing:.5px;background:linear-gradient(90deg,#ff2e93,#00e5ff);"
    "-webkit-background-clip:text;-webkit-text-fill-color:transparent;}"
    ".hdr-r{display:flex;align-items:center;gap:6px;}"
    ".lang-btn{font-size:12px;padding:6px 11px;border-radius:99px;background:#1b2236;border:1px solid #4a5578;color:#fff;cursor:pointer;}"
    ".badge{font-size:12px;padding:5px 10px;border-radius:99px;background:#1e2436;border:1px solid #353f5e;color:#9ab;font-weight:700;}"
    ".badge.ok{background:#093829;border-color:#00e676;color:#69f0ae;}"
    ".badge.warn{background:#3e2709;border-color:#ffab00;color:#ffd740;}"
    ".card{background:rgba(22,27,44,0.92);border:1px solid rgba(255,255,255,0.09);border-radius:18px;padding:12px;margin:8px 0;"
    "box-shadow:0 8px 24px rgba(0,0,0,0.4);}"
    ".row{display:flex;justify-content:center;align-items:center;gap:8px;margin:6px 0;}"
    "select,button{font-family:inherit;font-weight:800;border-radius:14px;border:2px solid #2e3856;"
    "background:#1b2236;color:#fff;padding:12px 14px;font-size:15px;transition:transform .06s,background .1s;}"
    "button:active,button.act{transform:scale(0.95);background:#ff2e93;border-color:#ff80bf;color:#fff;}"
    ".btn-start{background:linear-gradient(135deg,#ff2e93,#ff6b00);border:none;color:#fff;padding:13px 16px;font-size:15px;flex:1;}"
    ".bars{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:8px;font-size:11px;color:#9ab;}"
    ".bar-box{background:#0f1320;border-radius:10px;padding:6px;border:1px solid #232b42;}"
    ".bar-val{font-size:14px;font-weight:800;color:#00e5ff;display:block;margin-top:2px;}"
    ".warn-box{display:none;background:#3b1422;border:1px solid #ff4081;border-radius:14px;padding:10px;margin:8px 0;"
    "font-size:12px;line-height:1.45;text-align:left;color:#ffcdd2;}"
    ".warn-box code{background:#1a0910;padding:2px 5px;border-radius:4px;color:#80d8ff;word-break:break-all;}"
    ".move-big{width:100%;padding:22px 12px;font-size:19px;border-radius:16px;background:linear-gradient(135deg,#0d3b52,#162447);"
    "border:2px solid #00e5ff;box-shadow:0 0 18px rgba(0,229,255,0.22);margin-bottom:4px;}"
    ".move-big:active,.move-big.act{background:#00e5ff;color:#08101a;}"
    ".sec-lbl{font-size:11px;text-transform:uppercase;letter-spacing:1px;color:#7e8ba8;margin:0 0 8px;}"
    ".dpad-big{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;max-width:310px;margin:6px auto;}"
    ".dpad-big button{height:64px;font-size:24px;border-radius:16px;padding:0;display:flex;align-items:center;justify-content:center;"
    "background:#1f2740;border:2px solid #3c4a78;}"
    ".dpad-big .btn-ok{background:linear-gradient(135deg,#1e3c72,#2a5298);border-color:#448aff;font-size:18px;color:#fff;}"
    ".act-grid{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:10px;}"
    ".act-grid button{padding:15px 10px;font-size:15px;border-radius:14px;}"
    ".b-x{border-color:#448aff;color:#82b1ff;} .b-o{border-color:#ff5252;color:#ff8a80;}"
    ".b-t{border-color:#00e676;color:#69f0ae;} .b-s{border-color:#e040fb;color:#ea80fc;}"
    ".sys-row{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;margin-top:8px;}"
    ".sys-row button{padding:11px 4px;font-size:12px;border-color:#3a4466;color:#b0bec5;}"
    "</style></head><body>"
    "<div class='hdr'>"
    "<div class='title'>JUST DANCE PS5</div>"
    "<div class='hdr-r'>"
    "<div id='bdg' class='badge warn'>Sensors Idle</div>"
    "<button id='lbtn' class='lang-btn' onclick='toggleLang()'>PT-BR</button>"
    "</div>"
    "</div>"

    "<div class='card'>"
    "<div class='row'>"
    "<select id='pl' onchange='sendNow(true)'>"
    "<option value='0' id='opt0'>Player 1</option><option value='1' id='opt1'>Player 2</option>"
    "<option value='2' id='opt2'>Player 3</option><option value='3' id='opt3'>Player 4</option>"
    "</select>"
    "<button id='bstart' class='btn-start' onclick='startSensors()'>ENABLE SENSORS</button>"
    "</div>"
    "<div class='row' style='font-size:12px;color:#b0bec5;justify-content:space-between;padding:2px 4px 0;'>"
    "<span><span id='lsens'>Sensitivity</span>: <b id='sv'>1.5x</b></span>"
    "<input id='sens' type='range' min='0.8' max='3.0' step='0.1' value='1.5' style='width:55%' "
    "oninput=\"document.getElementById('sv').innerText=this.value+'x'\">"
    "</div>"
    "<div class='bars'>"
    "<div class='bar-box'>Accel X (G)<span id='vx' class='bar-val'>0.00</span></div>"
    "<div class='bar-box'>Accel Y (G)<span id='vy' class='bar-val'>0.00</span></div>"
    "<div class='bar-box'>Accel Z (G)<span id='vz' class='bar-val'>1.00</span></div>"
    "</div>"
    "<div id='st' style='font-size:12px;color:#80d8ff;margin-top:6px;'>Tap ENABLE SENSORS to start</div>"
    "</div>"

    "<div id='wbox' class='warn-box'>"
    "<div id='wtxt'></div>"
    "</div>"

    "<div class='card'>"
    "<button class='move-big' id='bmv' ontouchstart='mb(6,1,this,event)' ontouchend='mb(6,0,this,event)' "
    "onmousedown='mb(6,1,this,event)' onmouseup='mb(6,0,this,event)'>HOLD: MOVE + TRIGGER (T)</button>"

    "<div id='sec2' class='sec-lbl' style='margin-top:10px;'>Navigation &amp; Dancer Selection</div>"
    "<div class='dpad-big'>"
    "<div></div>"
    "<button ontouchstart='pb(16,0,1,this,event)' ontouchend='pb(16,0,0,this,event)' onmousedown='pb(16,0,1,this,event)' onmouseup='pb(16,0,0,this,event)'>▲</button>"
    "<div></div>"
    "<button ontouchstart='pb(128,0,1,this,event)' ontouchend='pb(128,0,0,this,event)' onmousedown='pb(128,0,1,this,event)' onmouseup='pb(128,0,0,this,event)'>◀</button>"
    "<button class='btn-ok' ontouchstart='pb(16384,68,1,this,event)' ontouchend='pb(16384,68,0,this,event)' onmousedown='pb(16384,68,1,this,event)' onmouseup='pb(16384,68,0,this,event)'>✖ OK</button>"
    "<button ontouchstart='pb(32,0,1,this,event)' ontouchend='pb(32,0,0,this,event)' onmousedown='pb(32,0,1,this,event)' onmouseup='pb(32,0,0,this,event)'>▶</button>"
    "<div></div>"
    "<button ontouchstart='pb(64,0,1,this,event)' ontouchend='pb(64,0,0,this,event)' onmousedown='pb(64,0,1,this,event)' onmouseup='pb(64,0,0,this,event)'>▼</button>"
    "<div></div>"
    "</div>"

    "<div class='act-grid'>"
    "<button class='b-x' id='bx' ontouchstart='pb(16384,68,1,this,event)' ontouchend='pb(16384,68,0,this,event)' onmousedown='pb(16384,68,1,this,event)' onmouseup='pb(16384,68,0,this,event)'>✖ Confirm</button>"
    "<button class='b-o' id='bo' ontouchstart='pb(8192,32,1,this,event)' ontouchend='pb(8192,32,0,this,event)' onmousedown='pb(8192,32,1,this,event)' onmouseup='pb(8192,32,0,this,event)'>● Back</button>"
    "<button class='b-t' id='bt' ontouchstart='pb(4096,16,1,this,event)' ontouchend='pb(4096,16,0,this,event)' onmousedown='pb(4096,16,1,this,event)' onmouseup='pb(4096,16,0,this,event)'>▲ Triangle</button>"
    "<button class='b-s' id='bs' ontouchstart='pb(32768,128,1,this,event)' ontouchend='pb(32768,128,0,this,event)' onmousedown='pb(32768,128,1,this,event)' onmouseup='pb(32768,128,0,this,event)'>■ Square</button>"
    "</div>"

    "<div class='sys-row'>"
    "<button ontouchstart='pb(1024,4,1,this,event)' ontouchend='pb(1024,4,0,this,event)' onmousedown='pb(1024,4,1,this,event)' onmouseup='pb(1024,4,0,this,event)'>L1</button>"
    "<button ontouchstart='pb(2048,4,1,this,event)' ontouchend='pb(2048,4,0,this,event)' onmousedown='pb(2048,4,1,this,event)' onmouseup='pb(2048,4,0,this,event)'>R1</button>"
    "<button ontouchstart='pb(1,1,1,this,event)' ontouchend='pb(1,1,0,this,event)' onmousedown='pb(1,1,1,this,event)' onmouseup='pb(1,1,0,this,event)'>SELECT</button>"
    "<button ontouchstart='pb(8,8,1,this,event)' ontouchend='pb(8,8,0,this,event)' onmousedown='pb(8,8,1,this,event)' onmouseup='pb(8,8,0,this,event)'>START</button>"
    "</div>"
    "</div>"

    "<script>"
    "const I18N={"
    "en:{"
    "toggle:'PT-BR',player:'Player ',start:'ENABLE SENSORS',sens:'Sensitivity',"
    "idle:'Sensors Idle',active:'IMU Active (60Hz)',blocked:'IMU Blocked (HTTP)',"
    "stIdle:'Tap ENABLE SENSORS to start',"
    "stOk:'Sensors active! Hold phone in your right hand and dance!',"
    "stWarn:'Unlock motion sensor below to dance:',"
    "bmv:'HOLD: MOVE + TRIGGER (T)',"
    "sec2:'Navigation & Dancer Selection',"
    "bx:'✖ Confirm',bo:'● Back',bt:'▲ Triangle',bs:'■ Square',"
    "warn:'<b>WARNING: Browser blocked motion sensors over plain HTTP!</b><br>'"
    "+'• <b>Android (Chrome):</b> Open <code>chrome://flags/#unsafely-treat-insecure-origin-as-secure</code>, '"
    "+'add <code>'+location.origin+'</code>, set to <b>Enabled</b> and relaunch Chrome.<br>'"
    "+'• <b>App Alternative:</b> Use <b>MotionSource</b> or <b>DSUController</b> pointing to this IP on port <b>9052</b>.'"
    "},"
    "pt:{"
    "toggle:'EN',player:'Jogador ',start:'ATIVAR SENSORES',sens:'Sensibilidade',"
    "idle:'Sensores Parados',active:'IMU Ativo (60Hz)',blocked:'IMU Bloqueado (HTTP)',"
    "stIdle:'Toque em ATIVAR SENSORES para iniciar',"
    "stOk:'Sensores ativos! Segure na mao direita e dance!',"
    "stWarn:'Desbloqueie o sensor abaixo para dancar:',"
    "bmv:'SEGURAR: MOVE + GATILHO (T)',"
    "sec2:'Navegacao e Selecao de Dancarino',"
    "bx:'✖ Confirmar',bo:'● Voltar',bt:'▲ Triangulo',bs:'■ Quadrado',"
    "warn:'<b>ATENCAO: O navegador bloqueou os sensores por estar em HTTP!</b><br>'"
    "+'• <b>Android (Chrome):</b> Abra <code>chrome://flags/#unsafely-treat-insecure-origin-as-secure</code>, '"
    "+'adicione <code>'+location.origin+'</code>, mude para <b>Enabled</b> e reinicie o Chrome.<br>'"
    "+'• <b>Alternativa App:</b> Use o <b>MotionSource</b> ou <b>DSUController</b> apontando para este IP na porta <b>9052</b>.'"
    "}"
    "};"
    "let lang='en',sensorState=0;"
    "let ax=0,ay=0,az=9.81,gx=0,gy=0,gz=0,btn=0,pad=0,on=false,pkts=0,motEvents=0,inFlight=false;"
    "function applyLang(){"
    "let t=I18N[lang];"
    "document.getElementById('lbtn').innerText=t.toggle;"
    "for(let i=0;i<4;i++)document.getElementById('opt'+i).innerText=t.player+(i+1);"
    "document.getElementById('bstart').innerText=t.start;"
    "document.getElementById('lsens').innerText=t.sens;"
    "document.getElementById('bmv').innerText=t.bmv;"
    "document.getElementById('sec2').innerText=t.sec2;"
    "document.getElementById('bx').innerText=t.bx;"
    "document.getElementById('bo').innerText=t.bo;"
    "document.getElementById('bt').innerText=t.bt;"
    "document.getElementById('bs').innerText=t.bs;"
    "document.getElementById('wtxt').innerHTML=t.warn;"
    "let bdg=document.getElementById('bdg');"
    "if(sensorState===0){bdg.className='badge warn';bdg.innerText=t.idle;document.getElementById('st').innerText=t.stIdle;}"
    "else if(sensorState===1){bdg.className='badge ok';bdg.innerText=t.active;document.getElementById('st').innerText=t.stOk;}"
    "else{bdg.className='badge warn';bdg.innerText=t.blocked;document.getElementById('st').innerText=t.stWarn;}"
    "}"
    "function toggleLang(){lang=(lang==='en')?'pt':'en';applyLang();}"
    "function mb(m,d,el,ev){if(ev)ev.preventDefault();if(d){btn|=m;el.classList.add('act');}else{btn&=~m;el.classList.remove('act');}sendNow(true);}"
    "function pb(pm,mm,d,el,ev){if(ev)ev.preventDefault();if(d){pad|=pm;btn|=mm;el.classList.add('act');}else{pad&=~pm;btn&=~mm;el.classList.remove('act');}sendNow(true);}"
    "function onMot(e){"
    "let a=e.accelerationIncludingGravity||e.acceleration;"
    "if(a&&(a.x!==null||a.y!==null||a.z!==null)){"
    "motEvents++;"
    "let s=parseFloat(document.getElementById('sens').value)||1.5;"
    "ax=(a.x||0)*s;ay=(a.y||0)*s;az=(a.z||0)*s;"
    "}"
    "let r=e.rotationRate;"
    "if(r){let s=parseFloat(document.getElementById('sens').value)||1.5;gx=(r.beta||0)*s;gy=(r.gamma||0)*s;gz=(r.alpha||0)*s;}"
    "}"
    "function sendNow(force){"
    "if(inFlight&&!force)return;"
    "inFlight=true;"
    "let p=parseInt(document.getElementById('pl').value)||0;"
    "let q='/api/m?p='+p+'&ax='+ax.toFixed(2)+'&ay='+ay.toFixed(2)+'&az='+az.toFixed(2)"
    "+'&gx='+gx.toFixed(1)+'&gy='+gy.toFixed(1)+'&gz='+gz.toFixed(1)+'&btn='+btn+'&pad='+pad;"
    "fetch(q,{method:'GET',keepalive:true}).then(()=>{inFlight=false;}).catch(()=>{inFlight=false;});"
    "pkts++;"
    "if(pkts%4===0){"
    "document.getElementById('vx').innerText=(ax/9.81).toFixed(2);"
    "document.getElementById('vy').innerText=(ay/9.81).toFixed(2);"
    "document.getElementById('vz').innerText=(az/9.81).toFixed(2);"
    "}"
    "}"
    "function startSensors(){"
    "if(typeof DeviceMotionEvent!=='undefined'&&typeof DeviceMotionEvent.requestPermission==='function'){"
    "DeviceMotionEvent.requestPermission().then(r=>{if(r==='granted')window.addEventListener('devicemotion',onMot);}).catch(()=>{});"
    "}else{window.addEventListener('devicemotion',onMot);}"
    "if(!on){"
    "on=true;setInterval(()=>sendNow(false),16);"
    "setTimeout(()=>{"
    "if(motEvents>0){sensorState=1;document.getElementById('wbox').style.display='none';}"
    "else{sensorState=2;document.getElementById('wbox').style.display='block';}"
    "applyLang();"
    "},700);"
    "}"
    "}"
    "applyLang();"
    "setInterval(()=>{if(!on)sendNow(false);},250);"
    "</script></body></html>";

static const char k_http_ok[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n\r\n"
    "ok";

static bool parse_query_float(const char *q, const char *key, float *out) {
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(q, pat);
    if (p == NULL)
        return false;
    p += strlen(pat);
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p)
        return false;
    *out = v;
    return true;
}

static bool parse_http_query_motion(const char *req, float accel_scale, motion_sample_t *out) {
    const char *q = strstr(req, "/api/m?");
    if (q == NULL)
        return false;
    q += 7;

    float p = 0, ax = 0, ay = 0, az = 9.81f, gx = 0, gy = 0, gz = 0, btn = 0, pad = 0;
    parse_query_float(q, "p", &p);
    parse_query_float(q, "ax", &ax);
    parse_query_float(q, "ay", &ay);
    parse_query_float(q, "az", &az);
    parse_query_float(q, "gx", &gx);
    parse_query_float(q, "gy", &gy);
    parse_query_float(q, "gz", &gz);
    parse_query_float(q, "btn", &btn);
    parse_query_float(q, "pad", &pad);

    char json[256];
    int len = snprintf(json, sizeof(json),
                       "{\"p\":%d,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
                       "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"btn\":%u,\"pad\":%u,\"mode\":3}",
                       (int)p, ax, ay, az, gx, gy, gz, (unsigned int)btn, (unsigned int)pad);
    if (len <= 0)
        return false;
    return protocol_parse_packet((const uint8_t *)json, (size_t)len, accel_scale, out);
}

static void handle_http_client(int client_fd, float accel_scale) {
    char buf[2048];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    motion_sample_t sample;
    if (strstr(buf, "/api/m?") != NULL) {
        if (parse_http_query_motion(buf, accel_scale, &sample))
            apply_motion_sample(&sample);
        (void)!send(client_fd, k_http_ok, sizeof(k_http_ok) - 1, 0);
    } else if (strncmp(buf, "POST ", 5) == 0 || strstr(buf, "/api/motion") != NULL) {
        const char *body = strstr(buf, "\r\n\r\n");
        if (body != NULL)
            body += 4;
        else
            body = buf;
        if (protocol_parse_packet((const uint8_t *)body, strlen(body), accel_scale, &sample))
            apply_motion_sample(&sample);
        (void)!send(client_fd, k_http_ok, sizeof(k_http_ok) - 1, 0);
    } else {
        (void)!send(client_fd, k_phone_html, sizeof(k_phone_html) - 1, 0);
    }
    close(client_fd);
}

/* ------------------------------------------------------------------ */
/* Socket Helpers & Network Loop                                       */
/* ------------------------------------------------------------------ */

static int open_udp_socket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int open_tcp_socket(int port) {
    if (port <= 0)
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_dsu_subscription(int udp_fd, const char *server_ip, int dsu_port) {
    if (udp_fd < 0 || server_ip == NULL || server_ip[0] == '\0')
        return;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dsu_port);
    if (inet_pton(AF_INET, server_ip, &dst.sin_addr) != 1)
        return;

    uint8_t pkt[32];
    size_t len = protocol_build_dsu_request(pkt, sizeof(pkt), 0x5053354Au, -1);
    if (len > 0)
        (void)sendto(udp_fd, pkt, len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void *net_thread_main(void *arg) {
    (void)arg;
    int udp_fd = open_udp_socket(g_cfg.udp_port);
    int web_fd = open_tcp_socket(g_cfg.web_port);

    if (udp_fd >= 0)
        LOG("UDP motion receiver listening on port %d (JDM1 / DSU / JSON)", g_cfg.udp_port);
    else
        LOG("could not bind UDP port %d: %s", g_cfg.udp_port, strerror(errno));

    if (web_fd >= 0)
        LOG("Smartphone Web Controller listening on http://0.0.0.0:%d", g_cfg.web_port);

    uint64_t last_dsu_sub_ms = 0;

    while (!g_net_stop) {
        uint64_t now = monotonic_ms();
        if (g_cfg.dsu_server_ip[0] != '\0' && now - last_dsu_sub_ms >= 2000) {
            send_dsu_subscription(udp_fd, g_cfg.dsu_server_ip, g_cfg.dsu_port);
            last_dsu_sub_ms = now;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = -1;
        if (udp_fd >= 0) {
            FD_SET(udp_fd, &rfds);
            if (udp_fd > max_fd)
                max_fd = udp_fd;
        }
        if (web_fd >= 0) {
            FD_SET(web_fd, &rfds);
            if (web_fd > max_fd)
                max_fd = web_fd;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 8000 }; /* 8 ms */
        int r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
            continue;

        if (udp_fd >= 0 && FD_ISSET(udp_fd, &rfds)) {
            uint8_t pkt[1024];
            for (int burst = 0; burst < 32; burst++) {
                ssize_t n = recvfrom(udp_fd, pkt, sizeof(pkt), MSG_DONTWAIT, NULL, NULL);
                if (n <= 0)
                    break;
                motion_sample_t sample;
                if (protocol_parse_packet(pkt, (size_t)n, g_cfg.accel_scale, &sample))
                    apply_motion_sample(&sample);
            }
        }

        if (web_fd >= 0 && FD_ISSET(web_fd, &rfds)) {
            for (int burst = 0; burst < 8; burst++) {
                struct sockaddr_in caddr;
                socklen_t clen = sizeof(caddr);
                int client = accept(web_fd, (struct sockaddr *)&caddr, &clen);
                if (client < 0)
                    break;
                struct timeval ctv = { .tv_sec = 0, .tv_usec = 50000 }; /* 50 ms max */
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
                handle_http_client(client, g_cfg.accel_scale);
                break;
            }
        }
    }

    if (udp_fd >= 0)
        close(udp_fd);
    if (web_fd >= 0)
        close(web_fd);
    return NULL;
}

int net_start(const config_t *cfg) {
    g_cfg = *cfg;
    g_net_stop = false;
    init_controller_defaults();
    if (pthread_create(&g_net_thread, NULL, net_thread_main, NULL) != 0)
        return -1;
    g_net_running = true;
    return 0;
}

void net_stop(void) {
    if (!g_net_running)
        return;
    g_net_stop = true;
    pthread_join(g_net_thread, NULL);
    g_net_running = false;
}

bool net_snapshot_controllers(stub_controller_state_t out[SCE_MOVE_MAX_PLAYERS], uint64_t *out_total_packets) {
    uint64_t now_ms = monotonic_ms();
    bool active_or_changed = false;

    pthread_mutex_lock(&g_state_mutex);
    for (int i = 0; i < SCE_MOVE_MAX_PLAYERS; i++) {
        if (g_controllers[i].connected && g_last_seen_ms[i] > 0 &&
            now_ms - g_last_seen_ms[i] > (uint64_t)g_cfg.controller_timeout_ms) {
            g_controllers[i].connected = 0;
            g_controllers[i].buttons = 0;
            g_controllers[i].trigger = 0;
            g_controllers[i].pad_buttons = 0;
            g_dirty = true;
            LOG("virtual PS Move slot %d disconnected (inactivity timeout)", i);
        }
        if (g_controllers[i].connected)
            active_or_changed = true;
    }
    if (g_dirty) {
        active_or_changed = true;
        g_dirty = false;
    }
    memcpy(out, g_controllers, sizeof(stub_controller_state_t) * SCE_MOVE_MAX_PLAYERS);
    if (out_total_packets != NULL)
        *out_total_packets = g_total_packets;
    pthread_mutex_unlock(&g_state_mutex);

    return active_or_changed;
}
