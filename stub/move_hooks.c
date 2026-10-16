/*
    move_hooks.c - PS5JustDance
    Freestanding, position-independent hooks for libSceMove injected into
    Just Dance (PS4 ABI, x86-64). Presents up to 4 virtual PlayStation Move
    controllers driven by Joy-Con or Smartphone motion data written into
    g_stub_header.controllers[] by the PS5JustDance daemon.

    Compiled with -ffreestanding -mno-sse -mno-sse2 so that game XMM/FPU
    registers are never clobbered.

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <stddef.h>
#include <stdint.h>

#include "scemove.h"
#include "stub_abi.h"

#define SYNTHETIC_HANDLE_BASE 0x30B00100

typedef int (*fn_init_t)(void);
typedef int (*fn_term_t)(void);
typedef int (*fn_open_t)(int user_id, int type, int index);
typedef int (*fn_close_t)(int handle);
typedef int (*fn_get_device_info_t)(int handle, SceMoveDeviceInfo *info);
typedef int (*fn_read_state_latest_t)(int handle, SceMoveData *data);
typedef int (*fn_read_state_recent_t)(int handle, int64_t timestamp, SceMoveData *data, int32_t *out_count);
typedef int (*fn_set_light_sphere_t)(int handle, uint8_t r, uint8_t g, uint8_t b);
typedef int (*fn_reset_light_sphere_t)(int handle);
typedef int (*fn_set_vibration_t)(int handle, uint8_t motor);

typedef struct handle_entry {
    int handle;       /* <= 0 means unused */
    int user_id;
    int index;
    int is_synthetic;
    int32_t sample_counter;
    uint64_t last_timestamp_us;
} handle_entry_t;

/* ---- image header: must be at offset 0 of stub.bin ---- */

extern char __stub_end[];

#define HDR_SECTION __attribute__((section(".header"), used, aligned(16)))

int stub_hook_init(void);
int stub_hook_term(void);
int stub_hook_open(int user_id, int type, int index);
int stub_hook_close(int handle);
int stub_hook_get_device_info(int handle, SceMoveDeviceInfo *info);
int stub_hook_read_state_latest(int handle, SceMoveData *data);
int stub_hook_read_state_recent(int handle, int64_t timestamp, SceMoveData *data, int32_t *out_count);
int stub_hook_set_light_sphere(int handle, uint8_t r, uint8_t g, uint8_t b);
int stub_hook_reset_light_sphere(int handle);
int stub_hook_set_vibration(int handle, uint8_t motor);

HDR_SECTION stub_header_t g_stub_header = {
    .magic = STUB_MAGIC,
    .abi_version = STUB_ABI_VERSION,
    .flags = STUB_FLAG_PASSTHROUGH_REAL,
    .num_History = 1,
    .image_end = (uint64_t)(uintptr_t)__stub_end,
    .hook_init = (uint64_t)(uintptr_t)stub_hook_init,
    .hook_term = (uint64_t)(uintptr_t)stub_hook_term,
    .hook_open = (uint64_t)(uintptr_t)stub_hook_open,
    .hook_close = (uint64_t)(uintptr_t)stub_hook_close,
    .hook_get_device_info = (uint64_t)(uintptr_t)stub_hook_get_device_info,
    .hook_read_state_latest = (uint64_t)(uintptr_t)stub_hook_read_state_latest,
    .hook_read_state_recent = (uint64_t)(uintptr_t)stub_hook_read_state_recent,
    .hook_set_light_sphere = (uint64_t)(uintptr_t)stub_hook_set_light_sphere,
    .hook_reset_light_sphere = (uint64_t)(uintptr_t)stub_hook_reset_light_sphere,
    .hook_set_vibration = (uint64_t)(uintptr_t)stub_hook_set_vibration,
    .controllers = {
        { .accel_bits = { IEEE754_ZERO_BITS, IEEE754_ZERO_BITS, IEEE754_ONE_BITS } },
        { .accel_bits = { IEEE754_ZERO_BITS, IEEE754_ZERO_BITS, IEEE754_ONE_BITS } },
        { .accel_bits = { IEEE754_ZERO_BITS, IEEE754_ZERO_BITS, IEEE754_ONE_BITS } },
        { .accel_bits = { IEEE754_ZERO_BITS, IEEE754_ZERO_BITS, IEEE754_ONE_BITS } },
    },
};

#define HDR (&g_stub_header)
#define ORIG(type, field) ((type)(uintptr_t)HDR->field)

static handle_entry_t g_handles[SCE_MOVE_MAX_PLAYERS];
static volatile int g_lock;

/* ---- freestanding memory primitives ---- */

void *memcpy(void *dst, const void *src, size_t n) {
    void *ret = dst;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memset(void *dst, int c, size_t n) {
    void *ret = dst;
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dst;
}

/* ---- synchronization & diagnostics ---- */

static inline void lock_acquire(void) {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}

static inline void lock_release(void) {
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
}

static inline void stat_inc(volatile uint32_t *counter) {
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static void trace(uint32_t code, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t index = __atomic_fetch_add(&HDR->trace_head, 1, __ATOMIC_RELAXED) % STUB_TRACE_SLOTS;
    stub_trace_event_t *e = &HDR->trace[index];
    e->a = a;
    e->b = b;
    e->c = c;
    __atomic_store_n(&e->code, code, __ATOMIC_RELEASE);
}

/* ---- slot management (caller holds g_lock) ---- */

static int find_slot_by_handle_locked(int handle) {
    if (handle <= 0)
        return -1;
    for (int i = 0; i < SCE_MOVE_MAX_PLAYERS; i++) {
        if (g_handles[i].handle == handle)
            return i;
    }
    /* If the game passed a handle that wasn't seen in sceMoveOpen, map low handles or synthetic handles */
    if (handle >= SYNTHETIC_HANDLE_BASE && handle < SYNTHETIC_HANDLE_BASE + SCE_MOVE_MAX_PLAYERS)
        return handle - SYNTHETIC_HANDLE_BASE;
    return -1;
}

static int alloc_slot_locked(int user_id, int index) {
    /* Reuse existing slot for the same (user_id, index) if reopened */
    for (int i = 0; i < SCE_MOVE_MAX_PLAYERS; i++) {
        if (g_handles[i].handle > 0 && g_handles[i].user_id == user_id && g_handles[i].index == index)
            return i;
    }
    /* Prefer matching `index` when in range 0..3 and free */
    if (index >= 0 && index < SCE_MOVE_MAX_PLAYERS && g_handles[index].handle <= 0)
        return index;
    /* Otherwise take the first free slot */
    for (int i = 0; i < SCE_MOVE_MAX_PLAYERS; i++) {
        if (g_handles[i].handle <= 0)
            return i;
    }
    return 0;
}

static int slot_is_connected_locked(int slot) {
    if (slot < 0 || slot >= SCE_MOVE_MAX_PLAYERS)
        return 0;
    if (HDR->controllers[slot].connected != 0)
        return 1;
    if (slot == 0 && (HDR->flags & STUB_FLAG_ALWAYS_CONNECTED) != 0)
        return 1;
    return 0;
}

static void fill_move_sample_locked(int slot, uint64_t min_timestamp_us, SceMoveData *out) {
    const stub_controller_state_t *ctrl = &HDR->controllers[slot];
    handle_entry_t *h = &g_handles[slot];

    memset(out, 0, sizeof(*out));

    /* If neutral/uninitialized (all zero), provide 1G on Z so gravity is valid */
    if (ctrl->accel_bits[0] == 0 && ctrl->accel_bits[1] == 0 && ctrl->accel_bits[2] == 0) {
        out->accel_bits[0] = IEEE754_ZERO_BITS;
        out->accel_bits[1] = IEEE754_ZERO_BITS;
        out->accel_bits[2] = IEEE754_ONE_BITS;
    } else {
        out->accel_bits[0] = ctrl->accel_bits[0];
        out->accel_bits[1] = ctrl->accel_bits[1];
        out->accel_bits[2] = ctrl->accel_bits[2];
    }

    out->gyro_bits[0] = ctrl->gyro_bits[0];
    out->gyro_bits[1] = ctrl->gyro_bits[1];
    out->gyro_bits[2] = ctrl->gyro_bits[2];

    uint16_t btns = ctrl->buttons;
    uint16_t trig = ctrl->trigger;
    if (trig > 128)
        btns |= SCE_MOVE_BUTTON_T;
    else if ((btns & SCE_MOVE_BUTTON_T) != 0 && trig == 0)
        trig = 255;

    out->pad.digital_buttons = btns;
    out->pad.analog_t = trig;

    /* Extension port neutral centered sticks */
    out->ext.analog_right_x = 0x80;
    out->ext.analog_right_y = 0x80;
    out->ext.analog_left_x = 0x80;
    out->ext.analog_left_y = 0x80;

    /* Ensure strictly increasing timestamp in microseconds */
    uint64_t ts = ctrl->timestamp_us;
    if (ts <= h->last_timestamp_us)
        ts = h->last_timestamp_us + 2000; /* +2 ms */
    if (ts <= min_timestamp_us)
        ts = min_timestamp_us + 2000;
    h->last_timestamp_us = ts;

    out->timestamp = ts;
    out->counter = ++h->sample_counter;
    out->temperature_bits = IEEE754_ROOM_TEMP_BITS;
}

/* ---- hooked libSceMove entry points ---- */

int stub_hook_init(void) {
    stat_inc(&HDR->stat_hook_calls);
    int r = SCE_MOVE_OK;
    fn_init_t orig = ORIG(fn_init_t, orig_init);
    if (orig != NULL) {
        r = orig();
        if (r < 0 && r != SCE_MOVE_ERROR_ALREADY_INIT)
            r = SCE_MOVE_OK;
    }
    trace(STUB_TRACE_INIT, (uint32_t)r, 0, 0);
    return SCE_MOVE_OK;
}

int stub_hook_term(void) {
    stat_inc(&HDR->stat_hook_calls);
    lock_acquire();
    memset(g_handles, 0, sizeof(g_handles));
    lock_release();

    fn_term_t orig = ORIG(fn_term_t, orig_term);
    if (orig != NULL)
        (void)orig();
    return SCE_MOVE_OK;
}

int stub_hook_open(int user_id, int type, int index) {
    stat_inc(&HDR->stat_hook_calls);
    int orig_handle = -1;
    fn_open_t orig = ORIG(fn_open_t, orig_open);
    if (orig != NULL)
        orig_handle = orig(user_id, type, index);

    lock_acquire();
    int slot = alloc_slot_locked(user_id, index);
    int handle = orig_handle;
    int synthetic = 0;
    if (handle <= 0) {
        handle = SYNTHETIC_HANDLE_BASE + slot;
        synthetic = 1;
    }
    g_handles[slot].handle = handle;
    g_handles[slot].user_id = user_id;
    g_handles[slot].index = index;
    g_handles[slot].is_synthetic = synthetic;
    if (g_handles[slot].last_timestamp_us == 0)
        g_handles[slot].last_timestamp_us = 1000000ull;
    stat_inc(&HDR->stat_opens);
    lock_release();

    trace(STUB_TRACE_OPEN, (uint32_t)user_id, (uint32_t)index, (uint32_t)handle);
    return handle;
}

int stub_hook_close(int handle) {
    stat_inc(&HDR->stat_hook_calls);
    int is_synthetic = 1;

    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    if (slot >= 0) {
        is_synthetic = g_handles[slot].is_synthetic;
        memset(&g_handles[slot], 0, sizeof(g_handles[slot]));
    }
    lock_release();

    trace(STUB_TRACE_CLOSE, (uint32_t)handle, (uint32_t)slot, 0);
    fn_close_t orig = ORIG(fn_close_t, orig_close);
    if (!is_synthetic && orig != NULL)
        return orig(handle);
    return SCE_MOVE_OK;
}

int stub_hook_get_device_info(int handle, SceMoveDeviceInfo *info) {
    stat_inc(&HDR->stat_hook_calls);
    if (info == NULL)
        return SCE_MOVE_ERROR_INVALID_ARG;

    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    int connected = slot_is_connected_locked(slot);
    int is_synthetic = (slot >= 0) ? g_handles[slot].is_synthetic : 1;
    lock_release();

    if (connected) {
        info->sphere_radius_bits = IEEE754_SPHERE_RAD_BITS; /* 22.5 mm */
        info->accel_to_sphere_offset_bits[0] = IEEE754_ZERO_BITS;
        info->accel_to_sphere_offset_bits[1] = IEEE754_ZERO_BITS;
        info->accel_to_sphere_offset_bits[2] = IEEE754_ZERO_BITS;
        trace(STUB_TRACE_DEVICE_INFO, (uint32_t)handle, (uint32_t)slot, SCE_MOVE_OK);
        return SCE_MOVE_OK;
    }

    fn_get_device_info_t orig = ORIG(fn_get_device_info_t, orig_get_device_info);
    if (!is_synthetic && (HDR->flags & STUB_FLAG_PASSTHROUGH_REAL) != 0 && orig != NULL)
        return orig(handle, info);

    memset(info, 0, sizeof(*info));
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}

int stub_hook_read_state_latest(int handle, SceMoveData *data) {
    stat_inc(&HDR->stat_hook_calls);
    stat_inc(&HDR->stat_reads);
    if (data == NULL)
        return SCE_MOVE_ERROR_INVALID_ARG;

    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    int connected = slot_is_connected_locked(slot);
    int is_synthetic = (slot >= 0) ? g_handles[slot].is_synthetic : 1;
    uint16_t btns = 0;

    if (connected) {
        fill_move_sample_locked(slot, 0, data);
        btns = data->pad.digital_buttons;
        stat_inc(&HDR->stat_connected_reads);
        lock_release();
        trace(STUB_TRACE_READ_LATEST, (uint32_t)handle, btns, SCE_MOVE_OK);
        return SCE_MOVE_OK;
    }
    lock_release();

    fn_read_state_latest_t orig = ORIG(fn_read_state_latest_t, orig_read_state_latest);
    if (!is_synthetic && (HDR->flags & STUB_FLAG_PASSTHROUGH_REAL) != 0 && orig != NULL)
        return orig(handle, data);

    memset(data, 0, sizeof(*data));
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}

int stub_hook_read_state_recent(int handle, int64_t timestamp, SceMoveData *data, int32_t *out_count) {
    stat_inc(&HDR->stat_hook_calls);
    stat_inc(&HDR->stat_reads);
    if (timestamp < 0 || data == NULL || out_count == NULL)
        return SCE_MOVE_ERROR_INVALID_ARG;

    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    int connected = slot_is_connected_locked(slot);
    int is_synthetic = (slot >= 0) ? g_handles[slot].is_synthetic : 1;
    uint16_t btns = 0;

    if (connected) {
        uint32_t count = HDR->num_History;
        if (count < 1)
            count = 1;
        if (count > SCE_MOVE_MAX_STATE_NUM)
            count = SCE_MOVE_MAX_STATE_NUM;

        uint64_t min_ts = (uint64_t)timestamp;
        for (uint32_t i = 0; i < count; i++) {
            fill_move_sample_locked(slot, min_ts, &data[i]);
            min_ts = data[i].timestamp;
        }
        btns = data[0].pad.digital_buttons;
        *out_count = (int32_t)count;
        stat_inc(&HDR->stat_connected_reads);
        lock_release();
        trace(STUB_TRACE_READ_RECENT, (uint32_t)handle, btns, count);
        return SCE_MOVE_OK;
    }
    lock_release();

    fn_read_state_recent_t orig = ORIG(fn_read_state_recent_t, orig_read_state_recent);
    if (!is_synthetic && (HDR->flags & STUB_FLAG_PASSTHROUGH_REAL) != 0 && orig != NULL)
        return orig(handle, timestamp, data, out_count);

    memset(data, 0, sizeof(*data));
    *out_count = 0;
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}

int stub_hook_set_light_sphere(int handle, uint8_t r, uint8_t g, uint8_t b) {
    stat_inc(&HDR->stat_hook_calls);
    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    int connected = slot_is_connected_locked(slot);
    int is_synthetic = (slot >= 0) ? g_handles[slot].is_synthetic : 1;
    if (slot >= 0) {
        HDR->controllers[slot].led_r = r;
        HDR->controllers[slot].led_g = g;
        HDR->controllers[slot].led_b = b;
    }
    lock_release();

    trace(STUB_TRACE_LIGHT_SPHERE, (uint32_t)handle, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b, 0);
    if (connected)
        return SCE_MOVE_OK;

    fn_set_light_sphere_t orig = ORIG(fn_set_light_sphere_t, orig_set_light_sphere);
    if (!is_synthetic && (HDR->flags & STUB_FLAG_PASSTHROUGH_REAL) != 0 && orig != NULL)
        return orig(handle, r, g, b);
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}

int stub_hook_reset_light_sphere(int handle) {
    stat_inc(&HDR->stat_hook_calls);
    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    int connected = slot_is_connected_locked(slot);
    int is_synthetic = (slot >= 0) ? g_handles[slot].is_synthetic : 1;
    lock_release();

    if (connected)
        return SCE_MOVE_OK;

    fn_reset_light_sphere_t orig = ORIG(fn_reset_light_sphere_t, orig_reset_light_sphere);
    if (!is_synthetic && (HDR->flags & STUB_FLAG_PASSTHROUGH_REAL) != 0 && orig != NULL)
        return orig(handle);
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}

int stub_hook_set_vibration(int handle, uint8_t motor) {
    stat_inc(&HDR->stat_hook_calls);
    lock_acquire();
    int slot = find_slot_by_handle_locked(handle);
    int connected = slot_is_connected_locked(slot);
    int is_synthetic = (slot >= 0) ? g_handles[slot].is_synthetic : 1;
    if (slot >= 0)
        HDR->controllers[slot].vibration = motor;
    lock_release();

    trace(STUB_TRACE_VIBRATION, (uint32_t)handle, motor, 0);
    if (connected)
        return SCE_MOVE_OK;

    fn_set_vibration_t orig = ORIG(fn_set_vibration_t, orig_set_vibration);
    if (!is_synthetic && (HDR->flags & STUB_FLAG_PASSTHROUGH_REAL) != 0 && orig != NULL)
        return orig(handle, motor);
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}
