/*
    net.h - PS5JustDance
    UDP motion receiver (JDM1 / Cemuhook DSU / JSON), DSU client keepalive,
    and embedded HTTP Smartphone Web Controller server.

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_NET_H
#define PS5JD_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "stub_abi.h"

/* Starts the background UDP + HTTP receiver thread. Returns 0 on success. */
int net_start(const config_t *cfg);

/* Stops the background network thread and closes sockets. */
void net_stop(void);

/*
    Copies the current 4 virtual controller states into `out` (applying
    inactivity timeouts). Returns true if any controller is currently active
    or if the state changed since the last call.
*/
bool net_snapshot_controllers(stub_controller_state_t out[SCE_MOVE_MAX_PLAYERS], uint64_t *out_total_packets);

#endif
