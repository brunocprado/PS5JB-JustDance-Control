/*
    test_protocol.c - PS5JustDance
    Host unit tests for JDM1 binary packets, Cemuhook / DSU (DSUS/DSUC)
    packets, and Smartphone JSON packets.
*/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "protocol.h"

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

#define CHECK_NEAR(actual, expected, eps, what)                                       \
    do {                                                                              \
        checks++;                                                                     \
        double a_ = (double)(actual), e_ = (double)(expected);                        \
        if (fabs(a_ - e_) > (eps)) {                                                  \
            failures++;                                                               \
            printf("FAIL %s:%d %s: got %.5f expected %.5f\n", __FILE__, __LINE__,     \
                   what, a_, e_);                                                     \
        }                                                                             \
    } while (0)

static void test_jdm1_joycon_right(void) {
    jdm_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.magic = JDM_UDP_MAGIC;
    pkt.player_index = 1;
    pkt.source_flags = JDM_SOURCE_JOYCON_R;
    pkt.buttons = SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_CROSS;
    pkt.trigger = 200;
    pkt.accel[0] = 1.5f;  /* +X Joy-Con R -> +Y PS Move */
    pkt.accel[1] = -0.5f; /* +Y Joy-Con R -> -X PS Move (so -(-0.5) = +0.5) */
    pkt.accel[2] = 0.8f;  /* +Z -> +Z */
    pkt.gyro[0] = 2.0f;
    pkt.gyro[1] = -1.0f;
    pkt.gyro[2] = 0.5f;

    motion_sample_t s;
    CHECK_EQ(protocol_parse_packet((const uint8_t *)&pkt, sizeof(pkt), 1.0f, &s), 1, "parse JDM1 Joy-Con R");
    CHECK_EQ(s.player_index, 1, "player index 1");
    CHECK_EQ(s.buttons, SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_CROSS, "buttons");
    CHECK_EQ(s.trigger, 200, "trigger");
    CHECK_NEAR(s.accel_g[0], 0.5f, 1e-4, "Joy-Con R accel X");
    CHECK_NEAR(s.accel_g[1], 1.5f, 1e-4, "Joy-Con R accel Y");
    CHECK_NEAR(s.accel_g[2], 0.8f, 1e-4, "Joy-Con R accel Z");
    CHECK_NEAR(s.gyro_rads[0], 1.0f, 1e-4, "Joy-Con R gyro X");
    CHECK_NEAR(s.gyro_rads[1], 2.0f, 1e-4, "Joy-Con R gyro Y");
    CHECK_NEAR(s.gyro_rads[2], 0.5f, 1e-4, "Joy-Con R gyro Z");
}

static void test_dsu_packet(void) {
    uint8_t dsu[100];
    memset(dsu, 0, sizeof(dsu));

    /* DSUS header */
    dsu[0] = 'D'; dsu[1] = 'S'; dsu[2] = 'U'; dsu[3] = 'S';
    dsu[4] = (uint8_t)(1001 & 0xFF); dsu[5] = (uint8_t)(1001 >> 8);
    dsu[6] = 84; dsu[7] = 0; /* payload length = 84 -> total = 100 */
    /* event_type = 0x00100002 */
    dsu[16] = 0x02; dsu[17] = 0x00; dsu[18] = 0x10; dsu[19] = 0x00;
    dsu[20] = 2; /* slot 2 (Player 3) */
    dsu[21] = 2; /* state = connected */
    dsu[31] = 1; /* connected_flag = 1 */
    dsu[36] = 0x08; /* Options/Plus -> Start */
    dsu[37] = 0x40; /* Cross/B -> Cross */
    dsu[38] = 1;    /* Home -> Move */
    dsu[54] = 250;  /* ZR -> T trigger */

    uint32_t ax = float_to_bits(0.25f);
    uint32_t ay = float_to_bits(-0.75f);
    uint32_t az = float_to_bits(1.10f);
    uint32_t gx_deg = float_to_bits(180.0f); /* 180 deg/s == pi rad/s */
    memcpy(dsu + 76, &ax, 4);
    memcpy(dsu + 80, &ay, 4);
    memcpy(dsu + 84, &az, 4);
    memcpy(dsu + 88, &gx_deg, 4);

    uint32_t crc = protocol_crc32(dsu, sizeof(dsu));
    memcpy(dsu + 8, &crc, 4);

    motion_sample_t s;
    CHECK_EQ(protocol_parse_packet(dsu, sizeof(dsu), 1.0f, &s), 1, "parse DSUS packet with valid CRC");
    CHECK_EQ(s.player_index, 2, "DSU slot 2");
    CHECK_EQ(s.buttons, SCE_MOVE_BUTTON_START | SCE_MOVE_BUTTON_CROSS | SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_T,
             "DSU mapped buttons");
    CHECK_EQ(s.trigger, 250, "DSU trigger");
    CHECK_NEAR(s.accel_g[0], 0.25f, 1e-4, "DSU accel X");
    CHECK_NEAR(s.accel_g[1], -0.75f, 1e-4, "DSU accel Y");
    CHECK_NEAR(s.accel_g[2], 1.10f, 1e-4, "DSU accel Z");
    CHECK_NEAR(s.gyro_rads[0], 3.14159f, 1e-3, "DSU gyro 180 deg/s -> pi rad/s");

    /* Corrupt a byte and verify CRC rejection */
    dsu[76] ^= 0xFF;
    CHECK_EQ(protocol_parse_packet(dsu, sizeof(dsu), 1.0f, &s), 0, "reject corrupted DSU CRC");
}

static void test_phone_json(void) {
    const char *json = "{\"p\":0,\"ax\":9.80665,\"ay\":0.0,\"az\":-9.80665,\"gx\":90.0,\"gy\":0,\"gz\":0,\"btn\":68,\"mode\":3}";
    motion_sample_t s;
    CHECK_EQ(protocol_parse_packet((const uint8_t *)json, strlen(json), 1.0f, &s), 1, "parse phone JSON");
    CHECK_EQ(s.player_index, 0, "phone player 0");
    CHECK_EQ(s.buttons, SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_CROSS, "phone buttons");
    CHECK_NEAR(s.accel_g[0], 1.0f, 1e-3, "phone 9.80665 m/s^2 -> 1.0 G");
    CHECK_NEAR(s.accel_g[2], -1.0f, 1e-3, "phone -9.80665 m/s^2 -> -1.0 G");
    CHECK_NEAR(s.gyro_rads[0], 1.570796f, 1e-3, "phone 90 deg/s -> pi/2 rad/s");
}

int main(void) {
    test_jdm1_joycon_right();
    test_dsu_packet();
    test_phone_json();

    printf("test_protocol: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
