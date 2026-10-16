/*
    test_stub.c - PS5JustDance
    Loads the freestanding stub blob (build/stub.bin) at a high virtual
    address on the host and drives its libSceMove hooks (Init, Open,
    GetDeviceInfo, ReadStateLatest, ReadStateRecent, SetLightSphere,
    SetVibration, Close). Verifies SceMoveData binary layout, position
    independence, multi-player slot routing, and real Move passthrough.
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

#include "protocol.h"
#include "scemove.h"
#include "stub_abi.h"

#ifndef STUB_BIN_PATH
#error STUB_BIN_PATH must be defined
#endif

static int failures = 0;
static int checks = 0;

#define CHECK_EQ(actual, expected, what)                                              \
    do {                                                                              \
        checks++;                                                                     \
        long a_ = (long)(actual), e_ = (long)(expected);                              \
        if (a_ != e_) {                                                               \
            failures++;                                                               \
            printf("FAIL %s:%d %s: got 0x%lx expected 0x%lx\n", __FILE__, __LINE__,   \
                   what, a_, e_);                                                     \
        }                                                                             \
    } while (0)

/* ---- mock libSceMove ---- */

static int mock_open_ret = -1;
static int mock_real_reads = 0;

static int mock_init(void) { return 0; }
static int mock_term(void) { return 0; }
static int mock_open(int user_id, int type, int index) {
    (void)user_id; (void)type; (void)index;
    return mock_open_ret;
}
static int mock_close(int handle) { (void)handle; return 0; }
static int mock_get_device_info(int handle, SceMoveDeviceInfo *info) {
    (void)handle;
    memset(info, 0, sizeof(*info));
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}
static int mock_read_latest(int handle, SceMoveData *data) {
    (void)handle;
    mock_real_reads++;
    memset(data, 0, sizeof(*data));
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}
static int mock_read_recent(int handle, int64_t ts, SceMoveData *data, int32_t *out_count) {
    (void)handle; (void)ts;
    mock_real_reads++;
    memset(data, 0, sizeof(*data));
    *out_count = 0;
    return SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED;
}

/* ---- loaded stub ---- */

static uint8_t *image = NULL;
static stub_header_t *hdr = NULL;

typedef int (*init_fn)(void);
typedef int (*term_fn)(void);
typedef int (*open_fn)(int, int, int);
typedef int (*close_fn)(int);
typedef int (*get_device_info_fn)(int, SceMoveDeviceInfo *);
typedef int (*read_latest_fn)(int, SceMoveData *);
typedef int (*read_recent_fn)(int, int64_t, SceMoveData *, int32_t *);
typedef int (*set_light_fn)(int, uint8_t, uint8_t, uint8_t);
typedef int (*set_vib_fn)(int, uint8_t);

static init_fn h_init;
static term_fn h_term;
static open_fn h_open;
static close_fn h_close;
static get_device_info_fn h_get_device_info;
static read_latest_fn h_read_latest;
static read_recent_fn h_read_recent;
static set_light_fn h_set_light;
static set_vib_fn h_set_vib;

static void load_stub(uintptr_t hint) {
    FILE *f = fopen(STUB_BIN_PATH, "rb");
    if (!f) { perror("open stub.bin"); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) { perror("read"); exit(2); }
    fclose(f);

    stub_header_t tmp;
    memcpy(&tmp, data, sizeof(tmp));
    size_t image_size = (size_t)tmp.image_end;
    size_t map_size = (image_size + 0xFFF) & ~(size_t)0xFFF;
    image = mmap((void *)hint, map_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (image == MAP_FAILED) { perror("mmap"); exit(2); }
    memcpy(image, data, (size_t)size);
    free(data);

    hdr = (stub_header_t *)image;
    CHECK_EQ(hdr->magic, STUB_MAGIC, "stub magic");
    CHECK_EQ(hdr->abi_version, STUB_ABI_VERSION, "stub abi version");
    CHECK_EQ(image_size >= (size_t)size, 1, "image_end covers file contents");

    hdr->orig_init = (uint64_t)(uintptr_t)mock_init;
    hdr->orig_term = (uint64_t)(uintptr_t)mock_term;
    hdr->orig_open = (uint64_t)(uintptr_t)mock_open;
    hdr->orig_close = (uint64_t)(uintptr_t)mock_close;
    hdr->orig_get_device_info = (uint64_t)(uintptr_t)mock_get_device_info;
    hdr->orig_read_state_latest = (uint64_t)(uintptr_t)mock_read_latest;
    hdr->orig_read_state_recent = (uint64_t)(uintptr_t)mock_read_recent;

    h_init = (init_fn)(image + hdr->hook_init);
    h_term = (term_fn)(image + hdr->hook_term);
    h_open = (open_fn)(image + hdr->hook_open);
    h_close = (close_fn)(image + hdr->hook_close);
    h_get_device_info = (get_device_info_fn)(image + hdr->hook_get_device_info);
    h_read_latest = (read_latest_fn)(image + hdr->hook_read_state_latest);
    h_read_recent = (read_recent_fn)(image + hdr->hook_read_state_recent);
    h_set_light = (set_light_fn)(image + hdr->hook_set_light_sphere);
    h_set_vib = (set_vib_fn)(image + hdr->hook_set_vibration);
}

static void check_abi_layout(void) {
    CHECK_EQ(sizeof(SceMoveDeviceInfo), 16, "SceMoveDeviceInfo size");
    CHECK_EQ(sizeof(SceMoveButtonData), 4, "SceMoveButtonData size");
    CHECK_EQ(sizeof(SceMoveExtensionPortData), 20, "SceMoveExtensionPortData size");
    CHECK_EQ(sizeof(SceMoveData), 64, "SceMoveData size (64 bytes)");
    CHECK_EQ(__builtin_offsetof(SceMoveData, accel_bits), 0, "accel offset");
    CHECK_EQ(__builtin_offsetof(SceMoveData, gyro_bits), 12, "gyro offset");
    CHECK_EQ(__builtin_offsetof(SceMoveData, pad), 24, "pad offset");
    CHECK_EQ(__builtin_offsetof(SceMoveData, ext), 28, "ext offset");
    CHECK_EQ(__builtin_offsetof(SceMoveData, timestamp), 48, "timestamp offset");
    CHECK_EQ(__builtin_offsetof(SceMoveData, counter), 56, "counter offset");
    CHECK_EQ(__builtin_offsetof(SceMoveData, temperature_bits), 60, "temperature offset");
}

static void check_virtual_move_flow(void) {
    CHECK_EQ(h_init(), SCE_MOVE_OK, "sceMoveInit");

    int h0 = h_open(100, 0, 0);
    int h1 = h_open(101, 0, 1);
    CHECK_EQ(h0 > 0, 1, "player 1 handle > 0");
    CHECK_EQ(h1 > 0 && h1 != h0, 1, "player 2 distinct handle");
    CHECK_EQ(hdr->stat_opens, 2, "stat_opens == 2");

    /* Initially disconnected */
    SceMoveDeviceInfo info;
    SceMoveData state[4];
    int32_t count = -1;
    CHECK_EQ(h_get_device_info(h0, &info), SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED, "disconnected device info");
    CHECK_EQ(h_read_recent(h0, 0, state, &count), SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED, "disconnected read");
    CHECK_EQ(count, 0, "disconnected out_count == 0");

    /* Connect Player 1 via shared controller state (simulating daemon proc_write) */
    hdr->controllers[0].connected = 1;
    hdr->controllers[0].accel_bits[0] = float_to_bits(1.25f);
    hdr->controllers[0].accel_bits[1] = float_to_bits(-0.50f);
    hdr->controllers[0].accel_bits[2] = float_to_bits(0.95f);
    hdr->controllers[0].gyro_bits[0]  = float_to_bits(3.14f);
    hdr->controllers[0].gyro_bits[1]  = float_to_bits(-1.57f);
    hdr->controllers[0].gyro_bits[2]  = float_to_bits(0.25f);
    hdr->controllers[0].buttons = SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_CROSS | SCE_MOVE_BUTTON_T;
    hdr->controllers[0].trigger = 240;
    hdr->controllers[0].timestamp_us = 5000000ull;

    CHECK_EQ(h_get_device_info(h0, &info), SCE_MOVE_OK, "connected device info");
    CHECK_EQ(info.sphere_radius_bits, IEEE754_SPHERE_RAD_BITS, "sphere radius 22.5f");

    /* ReadStateLatest */
    memset(state, 0, sizeof(state));
    CHECK_EQ(h_read_latest(h0, &state[0]), SCE_MOVE_OK, "sceMoveReadStateLatest");
    CHECK_EQ(state[0].accel_bits[0], float_to_bits(1.25f), "accel X");
    CHECK_EQ(state[0].accel_bits[1], float_to_bits(-0.50f), "accel Y");
    CHECK_EQ(state[0].accel_bits[2], float_to_bits(0.95f), "accel Z");
    CHECK_EQ(state[0].gyro_bits[0], float_to_bits(3.14f), "gyro X");
    CHECK_EQ(state[0].pad.digital_buttons, SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_CROSS | SCE_MOVE_BUTTON_T, "buttons");
    CHECK_EQ(state[0].pad.analog_t, 240, "analog T");

    /* ReadStateRecent must advance timestamp beyond cutoff */
    uint64_t prev_ts = state[0].timestamp;
    CHECK_EQ(h_read_recent(h0, (int64_t)prev_ts, state, &count), SCE_MOVE_OK, "sceMoveReadStateRecent");
    CHECK_EQ(count, 1, "out_count == 1");
    CHECK_EQ(state[0].timestamp > prev_ts, 1, "timestamp strictly increases");
    CHECK_EQ(state[0].counter, 2, "sample counter increments");

    /* Light sphere and vibration feedback stored in shared header */
    CHECK_EQ(h_set_light(h0, 255, 40, 120), SCE_MOVE_OK, "set light sphere");
    CHECK_EQ(hdr->controllers[0].led_r, 255, "led_r recorded");
    CHECK_EQ(hdr->controllers[0].led_g, 40, "led_g recorded");
    CHECK_EQ(hdr->controllers[0].led_b, 120, "led_b recorded");

    CHECK_EQ(h_set_vib(h0, 180), SCE_MOVE_OK, "set vibration");
    CHECK_EQ(hdr->controllers[0].vibration, 180, "vibration recorded");

    /* Player 2 is still disconnected while Player 1 is connected */
    CHECK_EQ(h_read_latest(h1, &state[0]), SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED, "player 2 still disconnected");

    /* Verify STUB_FLAG_PASSTHROUGH_REAL when a real handle exists */
    hdr->flags |= STUB_FLAG_PASSTHROUGH_REAL;
    mock_open_ret = 77;
    int h2 = h_open(102, 0, 2);
    CHECK_EQ(h2, 77, "real handle returned when mock_open succeeds");
    mock_real_reads = 0;
    CHECK_EQ(h_read_latest(h2, &state[0]), SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED, "passthrough read delegates to orig");
    CHECK_EQ(mock_real_reads, 1, "orig_read_state_latest called once");
    CHECK_EQ(h_close(h2), SCE_MOVE_OK, "close h2");

    CHECK_EQ(h_close(h0), SCE_MOVE_OK, "close h0");
    CHECK_EQ(h_close(h1), SCE_MOVE_OK, "close h1");
    CHECK_EQ(h_term(), SCE_MOVE_OK, "term");
}

int main(void) {
    load_stub(0x200000000ull);
    check_abi_layout();
    check_virtual_move_flow();

    printf("test_stub: %d checks, %d failures (stub loaded at %p)\n", checks, failures, (void *)image);
    return failures ? 1 : 0;
}
