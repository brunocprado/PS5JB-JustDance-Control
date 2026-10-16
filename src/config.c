/*
    config.c - PS5JustDance
    Key=value parser for /data/PS5JustDance/config.ini

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "protocol.h"
#include "stub_abi.h"

static bool parse_bool(const char *value, bool fallback) {
    if (!strcasecmp(value, "1") || !strcasecmp(value, "true") || !strcasecmp(value, "yes") || !strcasecmp(value, "on"))
        return true;
    if (!strcasecmp(value, "0") || !strcasecmp(value, "false") || !strcasecmp(value, "no") || !strcasecmp(value, "off"))
        return false;
    return fallback;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static void set_flag(config_t *cfg, uint32_t flag, bool on) {
    if (on)
        cfg->stub_flags |= flag;
    else
        cfg->stub_flags &= ~flag;
}

static void config_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->stub_flags = STUB_FLAG_PASSTHROUGH_REAL;
    cfg->notifications = true;
    cfg->stats_logging = false;
    cfg->crash_catch = false;
    cfg->trace_log = false;
    cfg->hook_any_move_game = true;
    cfg->poll_ms = 1000;
    cfg->udp_port = JDM_DEFAULT_PORT;
    cfg->web_port = JDM_WEB_PORT;
    cfg->dsu_port = DSU_DEFAULT_PORT;
    cfg->controller_timeout_ms = 5000;
    cfg->accel_scale = 1.0f;
}

void config_parse(config_t *cfg, const char *text) {
    char *copy = strdup(text);
    if (copy == NULL)
        return;

    char *save = NULL;
    for (char *line = strtok_r(copy, "\r\n", &save); line != NULL; line = strtok_r(NULL, "\r\n", &save)) {
        line = trim(line);
        if (*line == '\0' || *line == ';' || *line == '#' || *line == '[')
            continue;
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        if (!strcasecmp(key, "always_connected"))
            set_flag(cfg, STUB_FLAG_ALWAYS_CONNECTED, parse_bool(value, false));
        else if (!strcasecmp(key, "passthrough_real"))
            set_flag(cfg, STUB_FLAG_PASSTHROUGH_REAL, parse_bool(value, true));
        else if (!strcasecmp(key, "notifications"))
            cfg->notifications = parse_bool(value, cfg->notifications);
        else if (!strcasecmp(key, "stats_logging"))
            cfg->stats_logging = parse_bool(value, cfg->stats_logging);
        else if (!strcasecmp(key, "crash_catch"))
            cfg->crash_catch = parse_bool(value, cfg->crash_catch);
        else if (!strcasecmp(key, "trace_log"))
            cfg->trace_log = parse_bool(value, cfg->trace_log);
        else if (!strcasecmp(key, "hook_any_move_game"))
            cfg->hook_any_move_game = parse_bool(value, cfg->hook_any_move_game);
        else if (!strcasecmp(key, "poll_ms")) {
            int ms = atoi(value);
            if (ms >= 100 && ms <= 10000)
                cfg->poll_ms = ms;
        } else if (!strcasecmp(key, "udp_port")) {
            int port = atoi(value);
            if (port > 0 && port <= 65535)
                cfg->udp_port = port;
        } else if (!strcasecmp(key, "web_port")) {
            int port = atoi(value);
            if (port >= 0 && port <= 65535)
                cfg->web_port = port;
        } else if (!strcasecmp(key, "dsu_port")) {
            int port = atoi(value);
            if (port > 0 && port <= 65535)
                cfg->dsu_port = port;
        } else if (!strcasecmp(key, "controller_timeout_ms")) {
            int ms = atoi(value);
            if (ms >= 500 && ms <= 60000)
                cfg->controller_timeout_ms = ms;
        } else if (!strcasecmp(key, "accel_scale")) {
            float sc = strtof(value, NULL);
            if (sc >= 0.1f && sc <= 5.0f)
                cfg->accel_scale = sc;
        } else if (!strcasecmp(key, "dsu_server_ip")) {
            snprintf(cfg->dsu_server_ip, sizeof(cfg->dsu_server_ip), "%s", value);
        } else if (!strcasecmp(key, "extra_title_id")) {
            snprintf(cfg->extra_title_id, sizeof(cfg->extra_title_id), "%s", value);
        }
    }
    free(copy);
}

void config_load(config_t *cfg) {
    config_defaults(cfg);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (f == NULL) {
        LOG("no config at %s, using defaults (udp=%d web=%d)", CONFIG_PATH, cfg->udp_port, cfg->web_port);
        return;
    }
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    config_parse(cfg, buf);
    LOG("config: flags=0x%x udp=%d web=%d dsu=%s:%d scale=%.2f", cfg->stub_flags, cfg->udp_port, cfg->web_port,
        cfg->dsu_server_ip[0] ? cfg->dsu_server_ip : "(passive)", cfg->dsu_port, cfg->accel_scale);
}
