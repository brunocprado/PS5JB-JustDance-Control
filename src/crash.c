/*
    crash.c - PS5JustDance
    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <machine/reg.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#include "crash.h"
#include "log.h"
#include "proc_util.h"
#include "stub_abi.h"

#define MAX_WATCHES 4

typedef struct watch {
    bool used;
    volatile bool stop;
    pid_t pid;
    intptr_t stub_base;
    size_t stub_size;
    pthread_t thread;
} watch_t;

static watch_t g_watches[MAX_WATCHES];
static pthread_mutex_t g_watch_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *trace_name(uint32_t code) {
    switch (code) {
        case STUB_TRACE_INIT:         return "sceMoveInit";
        case STUB_TRACE_OPEN:         return "sceMoveOpen";
        case STUB_TRACE_CLOSE:        return "sceMoveClose";
        case STUB_TRACE_DEVICE_INFO:  return "sceMoveGetDeviceInfo";
        case STUB_TRACE_READ_LATEST:  return "sceMoveReadStateLatest";
        case STUB_TRACE_READ_RECENT:  return "sceMoveReadStateRecent";
        case STUB_TRACE_LIGHT_SPHERE: return "sceMoveSetLightSphere";
        case STUB_TRACE_VIBRATION:    return "sceMoveSetVibration";
        default:                      return "?";
    }
}

static void log_trace_event(uint32_t seq, const stub_trace_event_t *ev) {
    LOG("  trace #%u: 0x%02x %s a=0x%x b=0x%x c=0x%x", seq, ev->code, trace_name(ev->code), ev->a, ev->b, ev->c);
}

int trace_dump_new(pid_t pid, intptr_t stub_base, uint32_t *seen, int budget) {
    if (stub_base == 0 || budget <= 0)
        return 0;
    stub_header_t hdr;
    if (proc_read(pid, stub_base, &hdr, sizeof(hdr)) != 0 || hdr.magic != STUB_MAGIC)
        return 0;

    uint32_t head = hdr.trace_head;
    uint32_t from = *seen;
    if (head - from > STUB_TRACE_SLOTS)
        from = head - STUB_TRACE_SLOTS;

    int logged = 0;
    static int read_events = 0;
    uint32_t i = from;
    for (; i != head && logged < budget; i++) {
        const stub_trace_event_t *ev = &hdr.trace[i % STUB_TRACE_SLOTS];
        if ((ev->code == STUB_TRACE_READ_RECENT || ev->code == STUB_TRACE_READ_LATEST) && ++read_events > 5)
            continue;
        log_trace_event(i, ev);
        logged++;
    }
    *seen = i;
    return logged;
}

static bool is_fatal_signal(int sig) {
    switch (sig) {
        case SIGSEGV:
        case SIGBUS:
        case SIGILL:
        case SIGFPE:
        case SIGABRT:
        case SIGSYS:
        case SIGTRAP:
            return true;
        default:
            return false;
    }
}

static void describe_address(const watch_t *w, uintptr_t addr, char *out, size_t out_size) {
    if (w->stub_base != 0 && addr >= (uintptr_t)w->stub_base && addr < (uintptr_t)w->stub_base + w->stub_size) {
        snprintf(out, out_size, "STUB+0x%lx", (unsigned long)(addr - (uintptr_t)w->stub_base));
        return;
    }
    size_t size = 0;
    struct kinfo_vmentry *map = vmmap_get(w->pid, &size);
    snprintf(out, out_size, "unmapped");
    if (map == NULL)
        return;
    VMMAP_FOREACH(e, map, size) {
        if (addr >= e->kve_start && addr < e->kve_end) {
            snprintf(out, out_size, "%s+0x%lx (map 0x%lx-0x%lx prot=%c%c%c)",
                     e->kve_path[0] ? e->kve_path : "[anon]", (unsigned long)(addr - e->kve_start),
                     (unsigned long)e->kve_start, (unsigned long)e->kve_end,
                     (e->kve_protection & KVME_PROT_READ) ? 'r' : '-',
                     (e->kve_protection & KVME_PROT_WRITE) ? 'w' : '-',
                     (e->kve_protection & KVME_PROT_EXEC) ? 'x' : '-');
            break;
        }
    }
    free(map);
}

static void dump_crash(const watch_t *w, int sig) {
    char where[320];
    LOG("======== CRASH: pid %d received signal %d ========", w->pid, sig);

    struct ptrace_lwpinfo info;
    memset(&info, 0, sizeof(info));
    if (pu_ptrace(PT_LWPINFO, w->pid, (caddr_t)&info, sizeof(info)) == 0) {
        LOG("thread %d \"%s\" flags=0x%x si_signo=%d si_code=%d si_addr=%p", info.pl_lwpid, info.pl_tdname,
            info.pl_flags, info.pl_siginfo.si_signo, info.pl_siginfo.si_code, info.pl_siginfo.si_addr);
        if (info.pl_flags & PL_FLAG_SI) {
            describe_address(w, (uintptr_t)info.pl_siginfo.si_addr, where, sizeof(where));
            LOG("fault address -> %s", where);
        }
    }

    struct reg r;
    memset(&r, 0, sizeof(r));
    if (pu_ptrace(PT_GETREGS, w->pid, (caddr_t)&r, 0) == 0) {
        describe_address(w, (uintptr_t)r.r_rip, where, sizeof(where));
        LOG("rip = 0x%016lx -> %s", (unsigned long)r.r_rip, where);
        LOG("rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx", (unsigned long)r.r_rax, (unsigned long)r.r_rbx,
            (unsigned long)r.r_rcx, (unsigned long)r.r_rdx);
        LOG("rsi=%016lx rdi=%016lx rbp=%016lx rsp=%016lx", (unsigned long)r.r_rsi, (unsigned long)r.r_rdi,
            (unsigned long)r.r_rbp, (unsigned long)r.r_rsp);
    }

    if (w->stub_base != 0) {
        stub_header_t hdr;
        if (proc_read(w->pid, w->stub_base, &hdr, sizeof(hdr)) == 0 && hdr.magic == STUB_MAGIC) {
            LOG("stub: hook_calls=%u opens=%u reads=%u connected_reads=%u", hdr.stat_hook_calls,
                hdr.stat_opens, hdr.stat_reads, hdr.stat_connected_reads);
            uint32_t n = hdr.trace_head < 24 ? hdr.trace_head : 24;
            for (uint32_t i = hdr.trace_head - n; i != hdr.trace_head; i++)
                log_trace_event(i, &hdr.trace[i % STUB_TRACE_SLOTS]);
        }
    }
    LOG("======== end of crash report ========");
}

static void *watch_thread(void *arg) {
    watch_t *w = arg;
    int status = 0;

    if (pu_ptrace(PT_ATTACH, w->pid, 0, 0) != 0)
        return NULL;
    if (waitpid(w->pid, &status, 0) < 0 || !WIFSTOPPED(status))
        return NULL;
    pu_ptrace(PT_CONTINUE, w->pid, (caddr_t)1, 0);

    for (;;) {
        pid_t r = waitpid(w->pid, &status, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        if (w->stop && sig == SIGSTOP) {
            pu_ptrace(PT_DETACH, w->pid, (caddr_t)1, 0);
            return NULL;
        }
        if (is_fatal_signal(sig))
            dump_crash(w, sig);
        if (pu_ptrace(PT_CONTINUE, w->pid, (caddr_t)1, sig) != 0)
            break;
    }
    return NULL;
}

int crash_watch_start(pid_t pid, intptr_t stub_base, size_t stub_size) {
    int rc = -1;
    pthread_mutex_lock(&g_watch_mutex);
    for (int i = 0; i < MAX_WATCHES; i++) {
        watch_t *w = &g_watches[i];
        if (w->used)
            continue;
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->pid = pid;
        w->stub_base = stub_base;
        w->stub_size = stub_size;
        if (pthread_create(&w->thread, NULL, watch_thread, w) != 0)
            w->used = false;
        else
            rc = 0;
        break;
    }
    pthread_mutex_unlock(&g_watch_mutex);
    return rc;
}

void crash_watch_stop(pid_t pid) {
    pthread_mutex_lock(&g_watch_mutex);
    for (int i = 0; i < MAX_WATCHES; i++) {
        watch_t *w = &g_watches[i];
        if (!w->used || w->pid != pid)
            continue;
        w->stop = true;
        kill(pid, SIGSTOP);
        pthread_join(w->thread, NULL);
        w->used = false;
    }
    pthread_mutex_unlock(&g_watch_mutex);
}
