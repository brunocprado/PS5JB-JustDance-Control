/*
    stub_abi.h - PS5JustDance
    Shared layout between the PS5 daemon (injector + UDP/DSU/Web receiver)
    and the freestanding hook stub injected into Just Dance.

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_STUB_ABI_H
#define PS5JD_STUB_ABI_H

#include <stdint.h>
#include "scemove.h"

#define STUB_MAGIC       0x314D444Au /* "JDM1" little-endian */
#define STUB_ABI_VERSION 1

/* Config flags (stub_header_t::flags) */
#define STUB_FLAG_ALWAYS_CONNECTED (1u << 0) /* report Player 1 connected even before first UDP packet */
#define STUB_FLAG_PASSTHROUGH_REAL (1u << 1) /* call real libSceMove when a slot has no virtual input */

/* Trace event codes (stub_trace_event_t::code) */
#define STUB_TRACE_INIT         0x01
#define STUB_TRACE_OPEN         0x02 /* a=userId, b=index, c=returned handle */
#define STUB_TRACE_CLOSE        0x03 /* a=handle */
#define STUB_TRACE_DEVICE_INFO  0x04 /* a=handle, b=slot, c=ret */
#define STUB_TRACE_READ_LATEST  0x05 /* a=handle, b=buttons, c=ret */
#define STUB_TRACE_READ_RECENT  0x06 /* a=handle, b=buttons, c=out_count */
#define STUB_TRACE_LIGHT_SPHERE 0x07 /* a=handle, b=(r<<16)|(g<<8)|b */
#define STUB_TRACE_VIBRATION    0x08 /* a=handle, b=motor */

#define STUB_TRACE_SLOTS 64

typedef struct stub_trace_event {
    uint32_t code;
    uint32_t a;
    uint32_t b;
    uint32_t c;
} stub_trace_event_t;

/*
    Per-player virtual PS Move state updated by the daemon via proc_write()
    and read by the in-game hooks.
*/
typedef struct stub_controller_state {
    uint32_t connected;        /* 1 = active virtual controller, 0 = inactive */
    uint32_t seq;              /* incremented by daemon on each motion update */
    uint32_t accel_bits[3];    /* IEEE-754 float[3]: X, Y, Z in G units */
    uint32_t gyro_bits[3];     /* IEEE-754 float[3]: X, Y, Z in rad/s */
    uint16_t buttons;          /* SCE_MOVE_BUTTON_* bitmask */
    uint16_t trigger;          /* 0..255 analog T trigger */
    uint64_t timestamp_us;     /* monotonic timestamp in microseconds */
    uint8_t led_r;             /* last sphere color requested by game */
    uint8_t led_g;
    uint8_t led_b;
    uint8_t vibration;         /* last vibration intensity requested by game */
    uint32_t reserved;
} stub_controller_state_t;

typedef struct stub_header {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t flags;            /* STUB_FLAG_* */
    uint32_t num_History;      /* number of samples to return in sceMoveReadStateRecent (default 1) */
    uint64_t image_end;        /* total size of the stub image in memory, including .bss */

    /* Hook entry points as offsets from the start of the stub image (set at link time) */
    uint64_t hook_init;
    uint64_t hook_term;
    uint64_t hook_open;
    uint64_t hook_close;
    uint64_t hook_get_device_info;
    uint64_t hook_read_state_latest;
    uint64_t hook_read_state_recent;
    uint64_t hook_set_light_sphere;
    uint64_t hook_reset_light_sphere;
    uint64_t hook_set_vibration;

    /* Real libSceMove functions in the game process (filled by injector, 0 if not resolved) */
    uint64_t orig_init;
    uint64_t orig_term;
    uint64_t orig_open;
    uint64_t orig_close;
    uint64_t orig_get_device_info;
    uint64_t orig_read_state_latest;
    uint64_t orig_read_state_recent;
    uint64_t orig_set_light_sphere;
    uint64_t orig_reset_light_sphere;
    uint64_t orig_set_vibration;

    /* Shared virtual controller states for up to 4 players (written by daemon) */
    stub_controller_state_t controllers[SCE_MOVE_MAX_PLAYERS];

    /* Diagnostics written by the stub, read by the daemon */
    uint32_t stat_hook_calls;
    uint32_t stat_opens;
    uint32_t stat_reads;
    uint32_t stat_connected_reads;

    /* Ring buffer of recent events for debugging / crash analysis */
    uint32_t trace_head;
    uint32_t trace_reserved[3];
    stub_trace_event_t trace[STUB_TRACE_SLOTS];
} stub_header_t;

#define STUB_HEADER_OFFSET_OF(field) ((uint64_t)__builtin_offsetof(stub_header_t, field))

#endif
