/*
    inject.c - PS5JustDance
    Injection of the libSceMove hook stub into a Just Dance process.

    How it works:
    1. Resolve the real libSceMove functions inside the game process
       (kernel_dynlib_dlsym / nid_encode).
    2. Scan the game's readable data segments for GOT/import slots that
       point to those libSceMove functions.
    3. Attach with ptrace, allocate an executable region via remote mmap,
       copy stub.bin with the original pointers filled in, and redirect the
       import slots to the stub's hooks.

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <machine/reg.h>

#include <ps5/kernel.h>
#include <ps5/mdbg.h>
#include <ps5/nid.h>

#include "inject.h"
#include "log.h"
#include "proc_util.h"

#ifndef STUB_BIN_PATH
#error STUB_BIN_PATH must point to the built stub.bin
#endif

__asm__(".section .rodata\n"
        ".balign 16\n"
        ".global stub_blob_start\n"
        "stub_blob_start:\n"
        ".incbin \"" STUB_BIN_PATH "\"\n"
        ".global stub_blob_end\n"
        "stub_blob_end:\n"
        ".byte 0\n"
        ".previous\n");

extern const uint8_t stub_blob_start[];
extern const uint8_t stub_blob_end[];

#define STUB_MAX_IMAGE_SIZE 0x10000
#define MMAP_ALIGN          0x4000

/* ------------------------------------------------------------------ */
/* libSceMove imports we hook                                          */
/* ------------------------------------------------------------------ */

enum {
    H_INIT,
    H_TERM,
    H_OPEN,
    H_CLOSE,
    H_GET_DEVICE_INFO,
    H_READ_LATEST,
    H_READ_RECENT,
    H_SET_LIGHT,
    H_RESET_LIGHT,
    H_SET_VIBRATION,
    H_COUNT
};

static const char *const hook_names[H_COUNT] = {
    "sceMoveInit",
    "sceMoveTerm",
    "sceMoveOpen",
    "sceMoveClose",
    "sceMoveGetDeviceInfo",
    "sceMoveReadStateLatest",
    "sceMoveReadStateRecent",
    "sceMoveSetLightSphere",
    "sceMoveResetLightSphere",
    "sceMoveSetVibration",
};

typedef struct move_funcs {
    intptr_t fn[H_COUNT];
} move_funcs_t;

typedef struct plan_item {
    int hook;
    intptr_t addr;
} plan_item_t;

typedef struct plan {
    plan_item_t items[INJECT_MAX_PATCHES];
    int count;
} plan_t;

static int plan_count_for(const plan_t *plan, int hook) {
    int n = 0;
    for (int i = 0; i < plan->count; i++) {
        if (plan->items[i].hook == hook)
            n++;
    }
    return n;
}

static int plan_add(plan_t *plan, int hook, intptr_t addr) {
    for (int i = 0; i < plan->count; i++) {
        if (plan->items[i].addr == addr)
            return 0;
    }
    if (plan->count >= INJECT_MAX_PATCHES)
        return -1;
    plan->items[plan->count].hook = hook;
    plan->items[plan->count].addr = addr;
    plan->count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Process memory access                                               */
/* ------------------------------------------------------------------ */

int proc_read(pid_t pid, intptr_t addr, void *buf, size_t len) {
    if (mdbg_copyout(pid, addr, buf, len) == 0)
        return 0;
    return kernel_proc_copyout(pid, addr, buf, len);
}

int proc_write(pid_t pid, intptr_t addr, const void *buf, size_t len) {
    if (mdbg_copyin(pid, buf, addr, len) == 0)
        return 0;
    return kernel_proc_copyin(pid, buf, addr, len);
}

static int proc_write_force(pid_t pid, intptr_t addr, const void *buf, size_t len) {
    int prot = kernel_get_vmem_protection(pid, addr, len);
    bool changed = false;
    if (prot >= 0 && (prot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE)) {
        if (kernel_set_vmem_protection(pid, addr, len, prot | PROT_READ | PROT_WRITE) == 0)
            changed = true;
    }
    int r = proc_write(pid, addr, buf, len);
    if (changed)
        kernel_set_vmem_protection(pid, addr, len, prot);
    return r;
}

static int proc_write_chunked(pid_t pid, intptr_t addr, const uint8_t *buf, size_t len) {
    const size_t chunk = 0x1000;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        if (proc_write_force(pid, addr + (intptr_t)off, buf + off, n) != 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module / Process Information                                        */
/* ------------------------------------------------------------------ */

int sceKernelGetAppInfo(pid_t pid, void *info);

int inject_get_title_id(pid_t pid, char out[16]) {
    uint8_t buf[0x200];
    memset(buf, 0, sizeof(buf));
    if (sceKernelGetAppInfo(pid, buf) != 0)
        return -1;

    for (size_t i = 0; i + 9 <= sizeof(buf); i++) {
        if (memcmp(buf + i, "CUSA", 4) != 0 && memcmp(buf + i, "PPSA", 4) != 0)
            continue;
        bool digits = true;
        for (int j = 4; j < 9; j++) {
            if (buf[i + j] < '0' || buf[i + j] > '9')
                digits = false;
        }
        if (!digits)
            continue;
        memcpy(out, buf + i, 9);
        out[9] = '\0';
        return 0;
    }
    return -1;
}

bool inject_process_alive(pid_t pid) {
    return kill(pid, 0) == 0 || errno == EPERM;
}

struct kinfo_vmentry *vmmap_get(pid_t pid, size_t *out_size) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid };
    size_t size = 0;
    if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
        return NULL;
    size += size / 4 + 4096;
    uint8_t *buf = malloc(size);
    if (buf == NULL)
        return NULL;
    if (sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
        free(buf);
        return NULL;
    }
    *out_size = size;
    return (struct kinfo_vmentry *)buf;
}

static bool find_module(pid_t pid, const char *const *names, int count, uint32_t *handle) {
    for (int i = 0; i < count; i++) {
        if (kernel_dynlib_handle(pid, names[i], handle) == 0)
            return true;
    }
    return false;
}

bool inject_has_scemove_loaded(pid_t pid) {
    static const char *const names[] = { "libSceMove.sprx", "libSceMove" };
    uint32_t handle = 0;
    return find_module(pid, names, 2, &handle);
}

static inject_result_t resolve_scemove(pid_t pid, move_funcs_t *m) {
    static const char *const names[] = { "libSceMove.sprx", "libSceMove" };
    uint32_t handle = 0;
    if (!find_module(pid, names, 2, &handle))
        return INJECT_NOT_READY;

    for (int i = 0; i < H_COUNT; i++) {
        char nid[12];
        intptr_t addr = kernel_dynlib_dlsym(pid, handle, hook_names[i]);
        if (addr == 0)
            addr = kernel_dynlib_resolve(pid, handle, nid_encode(hook_names[i], nid));
        m->fn[i] = addr;
    }

    if (m->fn[H_OPEN] == 0 || (m->fn[H_READ_RECENT] == 0 && m->fn[H_READ_LATEST] == 0)) {
        LOG("libSceMove loaded, but core sceMoveOpen/ReadState symbols could not be resolved");
        return INJECT_UNSUPPORTED;
    }
    return INJECT_OK;
}

/* ------------------------------------------------------------------ */
/* Import slot discovery                                               */
/* ------------------------------------------------------------------ */

#define SCAN_CHUNK       0x40000
#define SCAN_MAX_ENTRY   (64u * 1024 * 1024)
#define SCAN_BUDGET      (256u * 1024 * 1024)
#define SCAN_RANGE_LIMIT 0x18000000ull

static void scan_import_slots(pid_t pid, intptr_t eboot_base, const move_funcs_t *m, plan_t *plan) {
    size_t map_size = 0;
    struct kinfo_vmentry *map = vmmap_get(pid, &map_size);
    if (map == NULL) {
        LOG("could not read the memory map of pid %d", pid);
        return;
    }

    bool by_path = false;
    VMMAP_FOREACH(e, map, map_size) {
        if (strstr(e->kve_path, "eboot") != NULL)
            by_path = true;
    }

    uint8_t *chunk = malloc(SCAN_CHUNK);
    if (chunk == NULL) {
        free(map);
        return;
    }

    uint64_t budget = SCAN_BUDGET;
    VMMAP_FOREACH(e, map, map_size) {
        bool candidate;
        if (by_path)
            candidate = strstr(e->kve_path, "eboot") != NULL || strstr(e->kve_path, "/app0/") != NULL;
        else
            candidate = e->kve_start >= (uint64_t)eboot_base && e->kve_start < (uint64_t)eboot_base + SCAN_RANGE_LIMIT;
        if (!candidate)
            continue;
        if ((e->kve_protection & PROT_READ) == 0 || (e->kve_protection & PROT_EXEC) != 0)
            continue;

        uint64_t size = e->kve_end - e->kve_start;
        if (size > SCAN_MAX_ENTRY || size > budget)
            continue;
        budget -= size;

        for (uint64_t off = 0; off < size; off += SCAN_CHUNK) {
            size_t n = size - off < SCAN_CHUNK ? (size_t)(size - off) : SCAN_CHUNK;
            if (proc_read(pid, (intptr_t)(e->kve_start + off), chunk, n) != 0)
                continue;
            for (size_t i = 0; i + 8 <= n; i += 8) {
                uint64_t value;
                memcpy(&value, chunk + i, sizeof(value));
                if (value == 0)
                    continue;
                for (int h = 0; h < H_COUNT; h++) {
                    if (m->fn[h] != 0 && value == (uint64_t)m->fn[h])
                        plan_add(plan, h, (intptr_t)(e->kve_start + off + i));
                }
            }
        }
    }

    free(chunk);
    free(map);
}

/* ------------------------------------------------------------------ */
/* ptrace session with remote syscalls                                 */
/* ------------------------------------------------------------------ */

typedef struct rsession {
    pid_t pid;
    bool attached;
    bool escalated;
    uint64_t saved_authid;
    uint8_t saved_caps[16];
    struct reg saved_regs;
    intptr_t gadget;
} rsession_t;

int pu_ptrace(int request, pid_t pid, caddr_t addr, int data) {
    return (int)__syscall(SYS_ptrace, request, pid, addr, data);
}

static int escalate(rsession_t *rs) {
    static const uint8_t privcaps[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    pid_t me = getpid();
    rs->saved_authid = kernel_get_ucred_authid(me);
    if (rs->saved_authid == 0 || kernel_get_ucred_caps(me, rs->saved_caps) != 0)
        return -1;
    if (kernel_set_ucred_authid(me, 0x4800000000010003ull) != 0)
        return -1;
    if (kernel_set_ucred_caps(me, privcaps) != 0) {
        kernel_set_ucred_authid(me, rs->saved_authid);
        return -1;
    }
    rs->escalated = true;
    return 0;
}

static void deescalate(rsession_t *rs) {
    if (!rs->escalated)
        return;
    pid_t me = getpid();
    kernel_set_ucred_authid(me, rs->saved_authid);
    kernel_set_ucred_caps(me, rs->saved_caps);
    rs->escalated = false;
}

static int wait_stop(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFSTOPPED(status))
        return -1;
    return WSTOPSIG(status);
}

static intptr_t find_syscall_gadget(pid_t pid) {
    static const char *const names[] = { "libkernel.sprx", "libkernel_sys.sprx", "libkernel_web.sprx", "libkernel" };
    uint32_t handle = 0;
    if (!find_module(pid, names, 4, &handle))
        return 0;
    intptr_t base = kernel_dynlib_mapbase_addr(pid, handle);
    if (base == 0)
        return 0;

    size_t map_size = 0;
    struct kinfo_vmentry *map = vmmap_get(pid, &map_size);
    if (map == NULL)
        return 0;

    intptr_t found = 0;
    uint8_t *buf = malloc(0x10000 + 4);
    if (buf == NULL) {
        free(map);
        return 0;
    }

    VMMAP_FOREACH(e, map, map_size) {
        if (found != 0)
            break;
        if ((e->kve_protection & PROT_EXEC) == 0 || e->kve_start < (uint64_t)base ||
            e->kve_start >= (uint64_t)base + 0x1000000ull)
            continue;
        uint64_t size = e->kve_end - e->kve_start;
        if (size > 0x400000)
            size = 0x400000;
        for (uint64_t off = 0; off < size && found == 0; off += 0x10000) {
            size_t n = size - off < 0x10000 + 4 ? (size_t)(size - off) : 0x10000 + 4;
            if (proc_read(pid, (intptr_t)(e->kve_start + off), buf, n) != 0)
                continue;
            for (size_t i = 0; i + 3 <= n; i++) {
                if (buf[i] == 0x0F && buf[i + 1] == 0x05 && buf[i + 2] == 0x72) {
                    found = (intptr_t)(e->kve_start + off + i);
                    break;
                }
            }
        }
    }
    free(buf);
    free(map);
    return found;
}

int inject_raise_privileges(void) {
    rsession_t rs;
    memset(&rs, 0, sizeof(rs));
    return escalate(&rs);
}

static int session_begin(rsession_t *rs, pid_t pid) {
    memset(rs, 0, sizeof(*rs));
    rs->pid = pid;

    rs->gadget = find_syscall_gadget(pid);
    if (rs->gadget == 0) {
        LOG("could not find a syscall instruction in libkernel of pid %d", pid);
        return -1;
    }
    if (escalate(rs) != 0) {
        LOG("could not raise privileges for ptrace");
        return -1;
    }
    if (pu_ptrace(PT_ATTACH, pid, 0, 0) != 0) {
        LOG("PT_ATTACH failed: %s", strerror(errno));
        deescalate(rs);
        return -1;
    }
    rs->attached = true;
    if (wait_stop(pid) < 0) {
        pu_ptrace(PT_DETACH, pid, 0, 0);
        rs->attached = false;
        deescalate(rs);
        return -1;
    }
    if (pu_ptrace(PT_GETREGS, pid, (caddr_t)&rs->saved_regs, 0) != 0) {
        pu_ptrace(PT_DETACH, pid, 0, 0);
        rs->attached = false;
        deescalate(rs);
        return -1;
    }
    return 0;
}

static void session_end(rsession_t *rs) {
    if (rs->attached) {
        pu_ptrace(PT_SETREGS, rs->pid, (caddr_t)&rs->saved_regs, 0);
        pu_ptrace(PT_DETACH, rs->pid, 0, 0);
        rs->attached = false;
    }
    deescalate(rs);
}

static int remote_syscall(rsession_t *rs, int number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                          uint64_t a5, uint64_t *ret) {
    struct reg regs = rs->saved_regs;
    regs.r_rax = number;
    regs.r_rdi = (int64_t)a0;
    regs.r_rsi = (int64_t)a1;
    regs.r_rdx = (int64_t)a2;
    regs.r_r10 = (int64_t)a3;
    regs.r_r8 = (int64_t)a4;
    regs.r_r9 = (int64_t)a5;
    regs.r_rip = rs->gadget;
    regs.r_rflags &= ~1ll;

    if (pu_ptrace(PT_SETREGS, rs->pid, (caddr_t)&regs, 0) != 0)
        return -errno;

    int sig = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (pu_ptrace(PT_STEP, rs->pid, (caddr_t)1, 0) != 0)
            return -errno;
        sig = wait_stop(rs->pid);
        if (sig == SIGTRAP)
            break;
        if (sig < 0)
            return -ECHILD;
    }
    if (sig != SIGTRAP)
        return -EINTR;

    struct reg out;
    if (pu_ptrace(PT_GETREGS, rs->pid, (caddr_t)&out, 0) != 0)
        return -errno;
    if (out.r_rflags & 1)
        return -(int)out.r_rax;
    *ret = (uint64_t)out.r_rax;
    return 0;
}

static intptr_t remote_alloc_exec(rsession_t *rs, size_t size) {
    uint64_t addr = 0;
    int r = remote_syscall(rs, SYS_mmap, 0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON,
                           (uint64_t)-1, 0, &addr);
    if (r != 0) {
        r = remote_syscall(rs, SYS_mmap, 0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, (uint64_t)-1, 0,
                           &addr);
        if (r != 0)
            return 0;
    }

    int prot = kernel_get_vmem_protection(rs->pid, (intptr_t)addr, size);
    if (prot < 0 || (prot & PROT_EXEC) == 0 || (prot & PROT_WRITE) == 0) {
        if (kernel_set_vmem_protection(rs->pid, (intptr_t)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            uint64_t unused;
            remote_syscall(rs, SYS_munmap, addr, size, 0, 0, 0, 0, &unused);
            return 0;
        }
    }
    return (intptr_t)addr;
}

/* ------------------------------------------------------------------ */
/* Patching & Rollback                                                 */
/* ------------------------------------------------------------------ */

static uint64_t stub_hook_offset(const stub_header_t *hdr, int hook) {
    switch (hook) {
        case H_INIT:            return hdr->hook_init;
        case H_TERM:            return hdr->hook_term;
        case H_OPEN:            return hdr->hook_open;
        case H_CLOSE:           return hdr->hook_close;
        case H_GET_DEVICE_INFO: return hdr->hook_get_device_info;
        case H_READ_LATEST:     return hdr->hook_read_state_latest;
        case H_READ_RECENT:     return hdr->hook_read_state_recent;
        case H_SET_LIGHT:       return hdr->hook_set_light_sphere;
        case H_RESET_LIGHT:     return hdr->hook_reset_light_sphere;
        case H_SET_VIBRATION:   return hdr->hook_set_vibration;
    }
    return 0;
}

static int apply_plan(injection_t *inj, const plan_t *plan, const stub_header_t *hdr) {
    for (int i = 0; i < plan->count; i++) {
        const plan_item_t *item = &plan->items[i];
        uint64_t hook_addr = (uint64_t)inj->stub_base + stub_hook_offset(hdr, item->hook);
        uint8_t bytes[8];
        memcpy(bytes, &hook_addr, 8);

        inject_patch_t *rec = &inj->patches[inj->num_patches];
        rec->addr = item->addr;
        rec->len = 8;
        if (proc_read(inj->pid, item->addr, rec->original, 8) != 0)
            return -1;
        if (proc_write_force(inj->pid, item->addr, bytes, 8) != 0)
            return -1;
        inj->num_patches++;

        uint8_t check[8];
        if (proc_read(inj->pid, item->addr, check, 8) != 0 || memcmp(check, bytes, 8) != 0)
            return -1;
        LOG("patched %s slot at 0x%lx -> 0x%lx", hook_names[item->hook], (long)item->addr, (long)hook_addr);
    }
    return 0;
}

void inject_rollback(injection_t *inj) {
    if (inj == NULL)
        return;
    for (int i = inj->num_patches - 1; i >= 0; i--) {
        inject_patch_t *p = &inj->patches[i];
        if (proc_write_force(inj->pid, p->addr, p->original, p->len) != 0)
            LOG("rollback: could not restore 0x%lx", (long)p->addr);
    }
    inj->num_patches = 0;
    inj->active = false;
}

int inject_read_stats(const injection_t *inj, stub_header_t *out) {
    if (inj == NULL || !inj->active)
        return -1;
    if (proc_read(inj->pid, inj->stub_base, out, sizeof(*out)) != 0)
        return -1;
    return out->magic == STUB_MAGIC ? 0 : -1;
}

int inject_write_controllers(const injection_t *inj, const stub_controller_state_t controllers[SCE_MOVE_MAX_PLAYERS]) {
    if (inj == NULL || !inj->active || inj->stub_base == 0)
        return -1;
    intptr_t addr = inj->stub_base + (intptr_t)STUB_HEADER_OFFSET_OF(controllers);
    return proc_write(inj->pid, addr, controllers, sizeof(stub_controller_state_t) * SCE_MOVE_MAX_PLAYERS);
}

static bool stub_blob_valid(const stub_header_t *hdr, size_t blob_size) {
    return blob_size >= sizeof(*hdr) && hdr->magic == STUB_MAGIC && hdr->abi_version == STUB_ABI_VERSION &&
           hdr->image_end >= blob_size && hdr->image_end <= STUB_MAX_IMAGE_SIZE;
}

inject_result_t inject_stub(pid_t pid, uint32_t flags, injection_t *inj) {
    memset(inj, 0, sizeof(*inj));
    inj->pid = pid;

    size_t blob_size = (size_t)(stub_blob_end - stub_blob_start);
    stub_header_t hdr;
    if (blob_size < sizeof(hdr))
        return INJECT_FAILED;
    memcpy(&hdr, stub_blob_start, sizeof(hdr));
    if (!stub_blob_valid(&hdr, blob_size))
        return INJECT_FAILED;

    move_funcs_t move;
    memset(&move, 0, sizeof(move));
    inject_result_t res = resolve_scemove(pid, &move);
    if (res != INJECT_OK)
        return res;

    intptr_t eboot_base = kernel_dynlib_mapbase_addr(pid, 0);
    if (eboot_base == 0)
        return INJECT_NOT_READY;
    LOG("pid %d: eboot base 0x%lx, libSceMove sceMoveOpen=0x%lx", pid, (long)eboot_base, (long)move.fn[H_OPEN]);

    plan_t plan;
    memset(&plan, 0, sizeof(plan));
    scan_import_slots(pid, eboot_base, &move, &plan);
    for (int h = 0; h < H_COUNT; h++) {
        if (plan_count_for(&plan, h) > 0)
            LOG("scan: %s referenced by %d slot(s)", hook_names[h], plan_count_for(&plan, h));
    }

    if (plan_count_for(&plan, H_OPEN) == 0 ||
        (plan_count_for(&plan, H_READ_RECENT) == 0 && plan_count_for(&plan, H_READ_LATEST) == 0)) {
        LOG("cannot find sceMoveOpen / sceMoveReadState imports in pid %d", pid);
        return INJECT_UNSUPPORTED;
    }

    rsession_t rs;
    if (session_begin(&rs, pid) != 0)
        return INJECT_FAILED;

    size_t alloc_size = (hdr.image_end + MMAP_ALIGN - 1) & ~((size_t)MMAP_ALIGN - 1);
    intptr_t base = remote_alloc_exec(&rs, alloc_size);
    if (base == 0) {
        session_end(&rs);
        return INJECT_FAILED;
    }
    inj->stub_base = base;
    inj->stub_size = alloc_size;

    uint8_t *image = calloc(1, blob_size);
    if (image == NULL) {
        session_end(&rs);
        return INJECT_FAILED;
    }
    memcpy(image, stub_blob_start, blob_size);
    stub_header_t *ihdr = (stub_header_t *)image;
    ihdr->flags = flags;
    ihdr->orig_init               = (uint64_t)move.fn[H_INIT];
    ihdr->orig_term               = (uint64_t)move.fn[H_TERM];
    ihdr->orig_open               = (uint64_t)move.fn[H_OPEN];
    ihdr->orig_close              = (uint64_t)move.fn[H_CLOSE];
    ihdr->orig_get_device_info    = (uint64_t)move.fn[H_GET_DEVICE_INFO];
    ihdr->orig_read_state_latest  = (uint64_t)move.fn[H_READ_LATEST];
    ihdr->orig_read_state_recent  = (uint64_t)move.fn[H_READ_RECENT];
    ihdr->orig_set_light_sphere   = (uint64_t)move.fn[H_SET_LIGHT];
    ihdr->orig_reset_light_sphere = (uint64_t)move.fn[H_RESET_LIGHT];
    ihdr->orig_set_vibration      = (uint64_t)move.fn[H_SET_VIBRATION];

    int werr = proc_write_chunked(pid, base, image, blob_size);
    stub_header_t verify;
    bool ok = werr == 0 && proc_read(pid, base, &verify, sizeof(verify)) == 0 && verify.magic == STUB_MAGIC;
    if (!ok) {
        LOG("could not copy stub into game process");
        free(image);
        session_end(&rs);
        return INJECT_FAILED;
    }
    LOG("stub (%zu bytes) installed at 0x%lx", blob_size, (long)base);

    inj->active = true;
    if (apply_plan(inj, &plan, ihdr) != 0) {
        LOG("patching failed, rolling back");
        inject_rollback(inj);
        free(image);
        session_end(&rs);
        return INJECT_FAILED;
    }

    free(image);
    session_end(&rs);
    return INJECT_OK;
}
