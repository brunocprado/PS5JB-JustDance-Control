/*
    crash.h - PS5JustDance
    Optional crash watcher and stub trace logger.
*/

#ifndef PS5JD_CRASH_H
#define PS5JD_CRASH_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int crash_watch_start(pid_t pid, intptr_t stub_base, size_t stub_size);
void crash_watch_stop(pid_t pid);
int trace_dump_new(pid_t pid, intptr_t stub_base, uint32_t *seen, int budget);

#endif
