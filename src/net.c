/*
    net.c - PS5JustDance
    Background network thread handling:
      - UDP port 9052 (JDM1 binary packets, Cemuhook DSUS packets, JSON packets)
      - Active Cemuhook DSUC subscription when dsu_server_ip is configured
      - HTTP port 8080 serving the built-in Smartphone Web Controller UI

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
    "Connection: close\r\n\r\n"
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
    "<title>PS5 Just Dance Controller</title>"
    "<style>"
    "body{margin:0;background:#10121a;color:#fff;font-family:sans-serif;text-align:center;user-select:none;}"
    "h2{margin:12px 0 4px;color:#ff2e93;} .sub{font-size:13px;color:#9aa;margin-bottom:10px;}"
    ".row{display:flex;justify-content:center;gap:10px;margin:10px;}"
    "button,select{background:#23283b;color:#fff;border:2px solid #ff2e93;border-radius:12px;"
    "padding:14px 18px;font-size:16px;font-weight:bold;}"
    "button:active{background:#ff2e93;}"
    ".big{width:85%;padding:22px;font-size:20px;background:#1c2235;border-color:#00e5ff;}"
    "#st{color:#00e5ff;font-size:14px;margin:8px;}"
    "</style></head><body>"
    "<h2>PS5 Just Dance</h2><div class='sub'>Emulador PS Move (Celular / Joy-Con)</div>"
    "<div class='row'>"
    "<select id='pl'><option value='0'>Jogador 1</option><option value='1'>Jogador 2</option>"
    "<option value='2'>Jogador 3</option><option value='3'>Jogador 4</option></select>"
    "<button onclick='startSensors()'>Ativar Sensores</button>"
    "</div>"
    "<div id='st'>Toque em Ativar Sensores e segure o celular na mao direita</div>"
    "<div class='row'><button class='big' ontouchstart='b(4,1)' ontouchend='b(4,0)' "
    "onmousedown='b(4,1)' onmouseup='b(4,0)'>MOVE / T (Gatilho)</button></div>"
    "<div class='row'>"
    "<button ontouchstart='b(64,1)' ontouchend='b(64,0)' onmousedown='b(64,1)' onmouseup='b(64,0)'>X (Confirmar)</button>"
    "<button ontouchstart='b(32,1)' ontouchend='b(32,0)' onmousedown='b(32,1)' onmouseup='b(32,0)'>O (Voltar)</button>"
    "</div>"
    "<div class='row'>"
    "<button ontouchstart='b(16,1)' ontouchend='b(16,0)' onmousedown='b(16,1)' onmouseup='b(16,0)'>Triangulo</button>"
    "<button ontouchstart='b(128,1)' ontouchend='b(128,0)' onmousedown='b(128,1)' onmouseup='b(128,0)'>Quadrado</button>"
    "<button ontouchstart='b(8,1)' ontouchend='b(8,0)' onmousedown='b(8,1)' onmouseup='b(8,0)'>START</button>"
    "</div>"
    "<script>"
    "let ax=0,ay=0,az=9.81,gx=0,gy=0,gz=0,btn=0,on=false,pkts=0;"
    "function b(m,d){if(d)btn|=(m===4?6:m);else btn&=~(m===4?6:m);sendNow();}"
    "function onMot(e){"
    "let a=e.accelerationIncludingGravity||e.acceleration;"
    "if(a){ax=a.x||0;ay=a.y||0;az=a.z||0;}"
    "let r=e.rotationRate;if(r){gx=r.beta||0;gy=r.gamma||0;gz=r.alpha||0;}"
    "}"
    "function sendNow(){"
    "let p=parseInt(document.getElementById('pl').value)||0;"
    "fetch('/api/motion',{method:'POST',body:JSON.stringify({p:p,ax:ax,ay:ay,az:az,gx:gx,gy:gy,gz:gz,btn:btn,mode:3})}).catch(()=>{});"
    "pkts++;if(pkts%20===0)document.getElementById('st').innerText='Enviando ('+pkts+' pkts) ax='+ax.toFixed(1);"
    "}"
    "function startSensors(){"
    "if(typeof DeviceMotionEvent!=='undefined'&&typeof DeviceMotionEvent.requestPermission==='function'){"
    "DeviceMotionEvent.requestPermission().then(r=>{if(r==='granted'){window.addEventListener('devicemotion',onMot);}});"
    "}else{window.addEventListener('devicemotion',onMot);}"
    "if(!on){on=true;setInterval(sendNow,20);document.getElementById('st').innerText='Sensores ativos! Bom jogo!';}"
    "}"
    "</script></body></html>";

static const char k_http_ok[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Content-Length: 11\r\n"
    "Connection: close\r\n\r\n"
    "{\"ok\":true}";

static void handle_http_client(int client_fd, float accel_scale) {
    char buf[2048];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    if (strncmp(buf, "POST ", 5) == 0 || strstr(buf, "/api/motion") != NULL) {
        const char *body = strstr(buf, "\r\n\r\n");
        if (body != NULL)
            body += 4;
        else
            body = buf;
        motion_sample_t sample;
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
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
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

        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 }; /* 20 ms */
        int r = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0)
            continue;

        if (udp_fd >= 0 && FD_ISSET(udp_fd, &rfds)) {
            uint8_t pkt[1024];
            for (int burst = 0; burst < 16; burst++) {
                ssize_t n = recvfrom(udp_fd, pkt, sizeof(pkt), MSG_DONTWAIT, NULL, NULL);
                if (n <= 0)
                    break;
                motion_sample_t sample;
                if (protocol_parse_packet(pkt, (size_t)n, g_cfg.accel_scale, &sample))
                    apply_motion_sample(&sample);
            }
        }

        if (web_fd >= 0 && FD_ISSET(web_fd, &rfds)) {
            int client = accept(web_fd, NULL, NULL);
            if (client >= 0) {
                struct timeval ctv = { .tv_sec = 0, .tv_usec = 250000 };
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
                handle_http_client(client, g_cfg.accel_scale);
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
