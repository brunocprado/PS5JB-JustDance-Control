/*
    config.h - PS5JustDance
    Optional settings read from /data/PS5JustDance/config.ini

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_CONFIG_H
#define PS5JD_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_PATH "/data/PS5JustDance/config.ini"

typedef struct config {
    uint32_t stub_flags;        /* STUB_FLAG_* from stub_abi.h */
    bool notifications;         /* show PS5 on-screen notifications */
    bool stats_logging;         /* periodically log stub and UDP statistics */
    bool crash_catch;           /* attach as debugger and log game crashes */
    bool trace_log;             /* log the stub's event trace */
    bool hook_any_move_game;    /* also hook any CUSA game whose param.sfo says Just Dance or loads libSceMove */
    int poll_ms;                /* process polling interval while idle (ms) */
    int udp_port;               /* UDP port for JDM1 / DSU / JSON packets (default 9052) */
    int web_port;               /* HTTP port for the smartphone web controller (default 8080, 0 = off) */
    int dsu_port;               /* Remote DSU server port when dsu_server_ip is set (default 26760) */
    int controller_timeout_ms;  /* disconnect virtual slot after inactivity (default 5000 ms) */
    float accel_scale;          /* accelerometer multiplier for scoring sensitivity (default 1.0) */
    char dsu_server_ip[64];     /* optional IP of PC/Android running BetterJoy / DSU server */
    char extra_title_id[16];    /* optional additional CUSA title ID */
} config_t;

void config_load(config_t *cfg);
void config_parse(config_t *cfg, const char *text);

#endif
