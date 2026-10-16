/*
    test_support.c - PS5JustDance
    Host unit tests for config.ini parsing, Just Dance CUSA title matching,
    and param.sfo parsing.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "stub_abi.h"
#include "versions.h"

/* Mock log_write for host tests */
void log_write(const char *fmt, ...) { (void)fmt; }

static int failures = 0;
static int checks = 0;

#define CHECK_EQ(actual, expected, what)                                              \
    do {                                                                              \
        checks++;                                                                     \
        long a_ = (long)(actual), e_ = (long)(expected);                              \
        if (a_ != e_) {                                                               \
            failures++;                                                               \
            printf("FAIL %s:%d %s: got %ld expected %ld\n", __FILE__, __LINE__,       \
                   what, a_, e_);                                                     \
        }                                                                             \
    } while (0)

static void test_titles(void) {
    CHECK_EQ(versions_is_target_title("CUSA27802", NULL), 1, "JD 2022 US");
    CHECK_EQ(versions_is_target_title("CUSA27792", NULL), 1, "JD 2022 EU");
    CHECK_EQ(versions_is_target_title("CUSA19621", NULL), 1, "JD 2021 US");
    CHECK_EQ(versions_is_target_title("CUSA15669", NULL), 1, "JD 2020 EU");
    CHECK_EQ(versions_is_target_title("CUSA08367", NULL), 1, "JD 2018 EU");
    CHECK_EQ(versions_is_target_title("CUSA02450", NULL), 1, "JD 2016 US");
    CHECK_EQ(versions_is_target_title("CUSA99999", NULL), 0, "unknown title");
    CHECK_EQ(versions_is_target_title("CUSA99999", "CUSA99999"), 1, "custom extra_title_id");
}

static void test_config(void) {
    config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    config_parse(&cfg,
                 "# comment\n"
                 "always_connected = 1\n"
                 "passthrough_real = 0\n"
                 "udp_port = 9999\n"
                 "web_port = 8081\n"
                 "dsu_server_ip = 192.168.1.50\n"
                 "dsu_port = 26760\n"
                 "accel_scale = 1.25\n"
                 "extra_title_id = CUSA88888\n");

    CHECK_EQ((cfg.stub_flags & STUB_FLAG_ALWAYS_CONNECTED) != 0, 1, "always_connected flag");
    CHECK_EQ((cfg.stub_flags & STUB_FLAG_PASSTHROUGH_REAL) != 0, 0, "passthrough_real cleared");
    CHECK_EQ(cfg.udp_port, 9999, "udp_port");
    CHECK_EQ(cfg.web_port, 8081, "web_port");
    CHECK_EQ(strcmp(cfg.dsu_server_ip, "192.168.1.50"), 0, "dsu_server_ip");
    CHECK_EQ(strcmp(cfg.extra_title_id, "CUSA88888"), 0, "extra_title_id");
    CHECK_EQ((int)(cfg.accel_scale * 100.0f), 125, "accel_scale");
}

static void test_sfo(void) {
    /* Minimal valid SFO with APP_VER = "01.02" and TITLE = "Just Dance 2022" */
    uint8_t sfo[128];
    memset(sfo, 0, sizeof(sfo));
    memcpy(sfo, "\0PSF\x01\x01\x00\x00", 8);
    sfo[8] = 52;  /* key_table at 52 */
    sfo[12] = 68; /* data_table at 68 */
    sfo[16] = 2;  /* 2 entries */

    /* Entry 0: APP_VER */
    sfo[20] = 0;  /* key_off 0 */
    sfo[24] = 6;  /* data_len 6 */
    sfo[32] = 0;  /* data_off 0 */

    /* Entry 1: TITLE */
    sfo[36] = 8;  /* key_off 8 */
    sfo[40] = 16; /* data_len 16 */
    sfo[48] = 8;  /* data_off 8 */

    memcpy(sfo + 52, "APP_VER\0TITLE\0", 14);
    memcpy(sfo + 68, "01.02\0\0\0Just Dance 2022\0", 24);

    char ver[16] = { 0 };
    char title[32] = { 0 };
    CHECK_EQ(versions_parse_sfo_app_version(sfo, sizeof(sfo), ver, sizeof(ver)), 0, "parse APP_VER");
    CHECK_EQ(strcmp(ver, "01.02"), 0, "APP_VER value");
    CHECK_EQ(versions_parse_sfo_key(sfo, sizeof(sfo), "TITLE", title, sizeof(title)), 0, "parse TITLE");
    CHECK_EQ(strcmp(title, "Just Dance 2022"), 0, "TITLE value");
}

int main(void) {
    test_titles();
    test_config();
    test_sfo();

    printf("test_support: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
