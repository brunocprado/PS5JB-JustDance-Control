/*
    inject.h - PS5JustDance
    Injects the libSceMove hook stub into a running Just Dance process and
    provides fast shared-memory updates for the 4 virtual PS Move slots.

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_INJECT_H
#define PS5JD_INJECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "stub_abi.h"

typedef enum inject_result {
    INJECT_OK = 0,
    INJECT_NOT_READY = -1,   /* game has not loaded libSceMove yet */
    INJECT_UNSUPPORTED = -2, /* game does not import sceMove functions */
    INJECT_FAILED = -3       /* ptrace / mmap / patch error */
} inject_result_t;

#define INJECT_MAX_PATCHES 32

typedef struct inject_patch {
    intptr_t addr;
    size_t len;
    uint8_t original[16];
} inject_patch_t;

typedef struct injection {
    bool active;
    pid_t pid;
    intptr_t stub_base;
    size_t stub_size;
    int num_patches;
    inject_patch_t patches[INJECT_MAX_PATCHES];
} injection_t;

/* True if the process currently has libSceMove loaded. */
bool inject_has_scemove_loaded(pid_t pid);

/* Injects the libSceMove hook stub into `pid`. */
inject_result_t inject_stub(pid_t pid, uint32_t flags, injection_t *inj);

/* Restores original imports when the game exits or the daemon stops. */
void inject_rollback(injection_t *inj);

/* Reads the stub header (stats and LED/vibration state). */
int inject_read_stats(const injection_t *inj, stub_header_t *out);

/* Writes the 4 virtual PS Move controller states into the stub inside `inj->pid`. */
int inject_write_controllers(const injection_t *inj, const stub_controller_state_t controllers[SCE_MOVE_MAX_PLAYERS]);

/* Reads the 9-character title ID (e.g. "CUSA27802") of `pid`. */
int inject_get_title_id(pid_t pid, char out[16]);

/* True if `pid` is still alive. */
bool inject_process_alive(pid_t pid);

#endif
