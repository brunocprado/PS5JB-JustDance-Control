/*
    main.c - PS5JustDance
    PS5 payload / etaHEN plugin that emulates up to 4 PlayStation Move
    controllers (libSceMove) in Just Dance (PS4 versions running on PS5),
    allowing Switch Joy-Cons and Smartphones to be used over UDP / DSU / Web.

    Based on the architecture of PS5Instrumentalizer and OrbisInstrumentalizer.
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>

#include <ps5/kernel.h>

#include "config.h"
#include "crash.h"
#include "inject.h"
#include "log.h"
#include "net.h"
#include "proc_util.h"
#include "versions.h"

#define PLUGIN_VERSION "1.0"

#define MAX_TARGETS          4
#define MAX_NOT_READY        1200 /* ~5 minutes at 250 ms */
#define MAX_HARD_FAILURES    120
#define FAST_POLL_MS         250
#define ACTIVE_SYNC_US       8000 /* 8 ms (~125 Hz) controller state push while in-game */
#define IDLE_SLEEP_US        50000
#define TRACE_BUDGET         300
#define CONTROL_PORT         9042 /* single-instance guard (9041 is PS5Instrumentalizer) */
#define ETAHEN_IPC_PORT      9028
#define ETAHEN_IPC_MAGIC     0xDEADBEEF
#define ETAHEN_JAILBREAK_CMD 6

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/* ------------------------------------------------------------------ */
/* PS5 On-Screen Notifications                                         */
/* ------------------------------------------------------------------ */

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static void notify(const config_t *cfg, const char *text) {
    LOG("%s", text);
    if (!cfg->notifications)
        return;
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

/* ------------------------------------------------------------------ */
/* etaHEN Self-Jailbreak                                               */
/* ------------------------------------------------------------------ */

typedef struct etahen_cmd {
    int magic;
    int cmd;
    int pid;
    int ret;
    char msg1[0x500];
    char msg2[0x500];
} etahen_cmd_t;

static void etahen_self_jailbreak(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ETAHEN_IPC_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG("etaHEN IPC not reachable (%s), assuming payload is already privileged", strerror(errno));
        close(fd);
        return;
    }

    etahen_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = (int)ETAHEN_IPC_MAGIC;
    cmd.cmd = ETAHEN_JAILBREAK_CMD;
    cmd.pid = getpid();
    cmd.ret = -1337;
    if (send(fd, &cmd, sizeof(cmd), 0) == (ssize_t)sizeof(cmd) && recv(fd, &cmd, sizeof(cmd), MSG_WAITALL) > 0)
        LOG("etaHEN jailbreak request returned %d", cmd.ret);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* Single Instance Guard                                               */
/* ------------------------------------------------------------------ */

static int bind_control_socket(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void ask_previous_instance_to_quit(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
        (void)!send(fd, "quit", 4, 0);
    close(fd);
}

static void control_poll(int fd) {
    if (fd < 0)
        return;
    struct timeval tv = { 0, 0 };
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    if (select(fd + 1, &set, NULL, NULL, &tv) > 0) {
        int client = accept(fd, NULL, NULL);
        if (client >= 0) {
            close(client);
            LOG("another instance asked this one to quit");
            g_stop = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Game Tracking                                                       */
/* ------------------------------------------------------------------ */

typedef struct tracked_game {
    bool used;
    pid_t pid;
    char title_id[16];
    char app_version[16];
    injection_t inj;
    int not_ready;
    int hard_failures;
    bool gave_up;
    stub_header_t last_stats;
    time_t last_stats_time;
    bool watching;
    uint32_t trace_seen;
    int trace_budget;
} tracked_game_t;

static tracked_game_t g_games[MAX_TARGETS];

static tracked_game_t *find_game(pid_t pid) {
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_games[i].used && g_games[i].pid == pid)
            return &g_games[i];
    }
    return NULL;
}

static tracked_game_t *add_game(pid_t pid, const char *title_id) {
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (!g_games[i].used) {
            memset(&g_games[i], 0, sizeof(g_games[i]));
            g_games[i].used = true;
            g_games[i].pid = pid;
            snprintf(g_games[i].title_id, sizeof(g_games[i].title_id), "%s", title_id);
            return &g_games[i];
        }
    }
    return NULL;
}

static void flush_trace(tracked_game_t *g) {
    if (g->inj.active && g->trace_budget > 0)
        g->trace_budget -= trace_dump_new(g->pid, g->inj.stub_base, &g->trace_seen, g->trace_budget);
}

static void forget_game(tracked_game_t *g) {
    if (g->watching)
        crash_watch_stop(g->pid);
    if (g->inj.active && inject_process_alive(g->pid))
        inject_rollback(&g->inj);
    memset(g, 0, sizeof(*g));
}

static bool is_target_game(pid_t pid, const char *title, const config_t *cfg) {
    if (versions_is_target_title(title, cfg->extra_title_id))
        return true;
    if (cfg->hook_any_move_game && strncmp(title, "CUSA", 4) == 0) {
        if (versions_is_just_dance_sfo_title(title))
            return true;
        if (inject_has_scemove_loaded(pid))
            return true;
    }
    return false;
}

static int find_target_processes(const config_t *cfg, pid_t *pids, char titles[][16], int max) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0 };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
        return 0;
    size += size / 8 + 4096;
    uint8_t *buf = malloc(size);
    if (buf == NULL)
        return 0;
    if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
        free(buf);
        return 0;
    }

    int found = 0;
    pid_t me = getpid();
    for (uint8_t *ptr = buf; ptr + sizeof(int) <= buf + size;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)ptr;
        if (ki->ki_structsize <= 0)
            break;
        ptr += ki->ki_structsize;
        if (ki->ki_pid == me || ki->ki_pid <= 1)
            continue;

        char title[16];
        if (inject_get_title_id(ki->ki_pid, title) != 0 || !is_target_game(ki->ki_pid, title, cfg))
            continue;
        if (found < max) {
            pids[found] = ki->ki_pid;
            snprintf(titles[found], 16, "%s", title);
            found++;
        }
    }
    free(buf);
    return found;
}

static void try_inject(tracked_game_t *g, const config_t *cfg) {
    if (g->app_version[0] == '\0') {
        if (versions_read_app_version(g->title_id, g->app_version, sizeof(g->app_version)) != 0)
            snprintf(g->app_version, sizeof(g->app_version), "unknown");
        LOG("%s pid %d version %s", g->title_id, g->pid, g->app_version);
    }

    inject_result_t r = inject_stub(g->pid, cfg->stub_flags, &g->inj);
    switch (r) {
        case INJECT_OK:
            notify(cfg, "PS5JustDance: Joy-Con / Phone Move support active!");
            g->trace_budget = cfg->trace_log ? TRACE_BUDGET : 0;
            if (cfg->crash_catch && crash_watch_start(g->pid, g->inj.stub_base, g->inj.stub_size) == 0)
                g->watching = true;
            break;
        case INJECT_NOT_READY:
            if (++g->not_ready >= MAX_NOT_READY) {
                g->gave_up = true;
                notify(cfg, "PS5JustDance: game never loaded libSceMove, giving up");
            } else if (g->not_ready == 1 || g->not_ready % 40 == 0) {
                LOG("pid %d: waiting for the game to load libSceMove (%d)", g->pid, g->not_ready);
            }
            break;
        case INJECT_UNSUPPORTED:
        case INJECT_FAILED:
            if (++g->hard_failures >= MAX_HARD_FAILURES) {
                g->gave_up = true;
                notify(cfg, "PS5JustDance: could not hook game (see log.txt)");
            } else if (g->hard_failures == 1 || g->hard_failures % 10 == 0) {
                LOG("pid %d: injection %s (attempt %d), will retry", g->pid,
                    r == INJECT_UNSUPPORTED ? "not possible yet" : "failed", g->hard_failures);
            }
            break;
    }
}

static void log_stats(tracked_game_t *g, uint64_t net_packets) {
    stub_header_t stats;
    if (inject_read_stats(&g->inj, &stats) != 0)
        return;
    if (stats.stat_hook_calls != g->last_stats.stat_hook_calls ||
        stats.stat_reads != g->last_stats.stat_reads) {
        LOG("pid %d stats: hook_calls=%u opens=%u reads=%u connected_reads=%u net_pkts=%lu",
            g->pid, stats.stat_hook_calls, stats.stat_opens, stats.stat_reads,
            stats.stat_connected_reads, (unsigned long)net_packets);
        g->last_stats = stats;
    }
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool scan_games(const config_t *cfg, uint64_t net_packets) {
    pid_t pids[MAX_TARGETS];
    char titles[MAX_TARGETS][16];
    int count = find_target_processes(cfg, pids, titles, MAX_TARGETS);

    for (int i = 0; i < MAX_TARGETS; i++) {
        tracked_game_t *g = &g_games[i];
        if (!g->used)
            continue;
        bool still_running = false;
        for (int j = 0; j < count; j++) {
            if (pids[j] == g->pid)
                still_running = true;
        }
        if (!still_running || !inject_process_alive(g->pid)) {
            LOG("pid %d (%s) exited", g->pid, g->title_id);
            forget_game(g);
        }
    }

    for (int j = 0; j < count; j++) {
        if (find_game(pids[j]) == NULL && add_game(pids[j], titles[j]) != NULL)
            LOG("found %s (pid %d)", titles[j], pids[j]);
    }

    bool pending = false;
    for (int i = 0; i < MAX_TARGETS; i++) {
        tracked_game_t *g = &g_games[i];
        if (!g->used || g->gave_up)
            continue;
        if (!g->inj.active) {
            try_inject(g, cfg);
            pending = !g->inj.active && !g->gave_up;
        } else if (cfg->stats_logging && time(NULL) - g->last_stats_time >= 5) {
            g->last_stats_time = time(NULL);
            log_stats(g, net_packets);
        }
    }

    return pending;
}

int main(void) {
    config_t cfg;

    log_init();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    LOG("version " PLUGIN_VERSION " starting (firmware 0x%08x, authid 0x%016lx)", kernel_get_fw_version(),
        (unsigned long)kernel_get_ucred_authid(getpid()));

    int control_fd = bind_control_socket();
    if (control_fd < 0) {
        LOG("an instance is already running, asking it to quit");
        ask_previous_instance_to_quit();
        for (int i = 0; i < 20 && control_fd < 0; i++) {
            usleep(250 * 1000);
            control_fd = bind_control_socket();
        }
        if (control_fd < 0) {
            LOG("the previous instance did not quit, exiting");
            return 1;
        }
    }

    config_load(&cfg);
    etahen_self_jailbreak();
    if (cfg.crash_catch && inject_raise_privileges() != 0) {
        LOG("could not raise privileges, crash watcher disabled");
        cfg.crash_catch = false;
    }

    if (net_start(&cfg) != 0)
        LOG("warning: could not start network receiver thread");

    char start_msg[160];
    snprintf(start_msg, sizeof(start_msg), "PS5JustDance loaded (UDP:%d / Web:%d)", cfg.udp_port, cfg.web_port);
    notify(&cfg, start_msg);

    long last_scan_ms = -1000000;
    long scan_interval_ms = cfg.poll_ms;
    stub_controller_state_t controllers[SCE_MOVE_MAX_PLAYERS];
    uint64_t net_packets = 0;

    while (!g_stop) {
        control_poll(control_fd);

        bool any_active_game = false;
        bool need_push = net_snapshot_controllers(controllers, &net_packets);

        for (int i = 0; i < MAX_TARGETS; i++) {
            tracked_game_t *g = &g_games[i];
            if (g->used && g->inj.active) {
                any_active_game = true;
                if (need_push)
                    (void)inject_write_controllers(&g->inj, controllers);
                flush_trace(g);
            }
        }

        if (now_ms() - last_scan_ms >= scan_interval_ms) {
            last_scan_ms = now_ms();
            bool pending = scan_games(&cfg, net_packets);
            scan_interval_ms = pending ? FAST_POLL_MS : cfg.poll_ms;
        }

        usleep(any_active_game ? ACTIVE_SYNC_US : IDLE_SLEEP_US);
    }

    LOG("stopping, restoring game imports");
    net_stop();
    for (int i = 0; i < MAX_TARGETS; i++) {
        if (g_games[i].used)
            forget_game(&g_games[i]);
    }
    close(control_fd);
    return 0;
}
