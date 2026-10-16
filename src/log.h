/*
    log.h - PS5JustDance
    Logging helpers. Writes to the kernel log (etaHEN port 9081) and appends
    to /data/PS5JustDance/log.txt.
*/

#ifndef PS5JD_LOG_H
#define PS5JD_LOG_H

#define PLUGIN_NAME "PS5JustDance"
#define LOG_DIR     "/data/PS5JustDance"
#define LOG_PATH    LOG_DIR "/log.txt"

void log_init(void);
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(fmt, ...) log_write(fmt, ##__VA_ARGS__)

#endif
