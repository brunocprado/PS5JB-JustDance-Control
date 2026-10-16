/*
    protocol.c - PS5JustDance
    Parsers and coordinate converters for:
      1. JDM1 native binary UDP packets
      2. Cemuhook / DSU (DSUS v1001) UDP packets (BetterJoy, DSUController, etc.)
      3. JSON motion packets (from the embedded Smartphone Web Controller)

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "protocol.h"

#define GRAVITY_MS2 9.80665f
#define DEG_TO_RAD  0.017453292519943295f

/* ------------------------------------------------------------------ */
/* IEEE 802.3 CRC32 (for Cemuhook / DSU protocol)                      */
/* ------------------------------------------------------------------ */

uint32_t protocol_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint16_t rd16_le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wr32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static float rd_float_le(const uint8_t *p) {
    return bits_to_float(rd32_le(p));
}

static float sanitize_float(float v, float fallback) {
    if (__builtin_isnan(v) || __builtin_isinf(v) || v < -1000.0f || v > 1000.0f)
        return fallback;
    return v;
}

/* ------------------------------------------------------------------ */
/* DSU (Cemuhook) Client Request Builder                               */
/* ------------------------------------------------------------------ */

size_t protocol_build_dsu_request(uint8_t *out_buf, size_t max_len, uint32_t client_id, int slot) {
    if (out_buf == NULL || max_len < 28)
        return 0;

    memset(out_buf, 0, 28);
    wr32_le(out_buf + 0, DSU_MAGIC_CLIENT);      /* "DSUC" */
    wr16_le(out_buf + 4, DSU_PROTOCOL_VERSION);  /* 1001 */
    wr16_le(out_buf + 6, 12);                    /* payload length after 16-byte header */
    wr32_le(out_buf + 8, 0);                     /* CRC32 placeholder */
    wr32_le(out_buf + 12, client_id);
    wr32_le(out_buf + 16, DSU_TYPE_CONTROLLER_DATA);

    if (slot >= 0 && slot < SCE_MOVE_MAX_PLAYERS) {
        out_buf[20] = 1; /* register by slot index */
        out_buf[21] = (uint8_t)slot;
    } else {
        out_buf[20] = 0; /* subscribe to all controllers */
    }

    uint32_t crc = protocol_crc32(out_buf, 28);
    wr32_le(out_buf + 8, crc);
    return 28;
}

/* ------------------------------------------------------------------ */
/* Coordinate Frame Transformations -> PS Move Frame                   */
/* ------------------------------------------------------------------ */

static void apply_source_transform(uint8_t source_flags, const float in_accel[3], const float in_gyro[3],
                                   float accel_scale, float out_accel[3], float out_gyro[3]) {
    float ax = sanitize_float(in_accel[0], 0.0f);
    float ay = sanitize_float(in_accel[1], 0.0f);
    float az = sanitize_float(in_accel[2], 1.0f);
    float gx = sanitize_float(in_gyro[0], 0.0f);
    float gy = sanitize_float(in_gyro[1], 0.0f);
    float gz = sanitize_float(in_gyro[2], 0.0f);

    if (accel_scale <= 0.0f)
        accel_scale = 1.0f;

    switch (source_flags) {
        case JDM_SOURCE_JOYCON_R:
            /*
                Switch Right Joy-Con IMU standard frame:
                  +X = up toward ZR, +Y = left toward rail, +Z = out of face buttons
                PS Move frame:
                  +X = right, +Y = up toward sphere, +Z = out of face buttons
            */
            out_accel[0] = -ay * accel_scale;
            out_accel[1] =  ax * accel_scale;
            out_accel[2] =  az * accel_scale;
            out_gyro[0]  = -gy;
            out_gyro[1]  =  gx;
            out_gyro[2]  =  gz;
            break;

        case JDM_SOURCE_JOYCON_L:
            /*
                Switch Left Joy-Con IMU standard frame:
                  -X = up toward ZL, +Y = right toward rail, +Z = out of face buttons
            */
            out_accel[0] =  ay * accel_scale;
            out_accel[1] = -ax * accel_scale;
            out_accel[2] =  az * accel_scale;
            out_gyro[0]  =  gy;
            out_gyro[1]  = -gx;
            out_gyro[2]  =  gz;
            break;

        case JDM_SOURCE_PHONE:
            /*
                W3C DeviceMotionEvent:
                  accelerationIncludingGravity in m/s^2 -> divide by 9.80665 for G
                  rotationRate in deg/s -> multiply by DEG_TO_RAD for rad/s
            */
            out_accel[0] = (ax / GRAVITY_MS2) * accel_scale;
            out_accel[1] = (ay / GRAVITY_MS2) * accel_scale;
            out_accel[2] = (az / GRAVITY_MS2) * accel_scale;
            out_gyro[0]  = gx * DEG_TO_RAD;
            out_gyro[1]  = gy * DEG_TO_RAD;
            out_gyro[2]  = gz * DEG_TO_RAD;
            break;

        case JDM_SOURCE_GENERIC:
        default:
            out_accel[0] = ax * accel_scale;
            out_accel[1] = ay * accel_scale;
            out_accel[2] = az * accel_scale;
            out_gyro[0]  = gx;
            out_gyro[1]  = gy;
            out_gyro[2]  = gz;
            break;
    }
}

/* ------------------------------------------------------------------ */
/* 1. JDM1 Binary Packet Parser                                        */
/* ------------------------------------------------------------------ */

static bool parse_jdm1(const uint8_t *buf, size_t len, float accel_scale, motion_sample_t *out) {
    if (len < sizeof(jdm_packet_t))
        return false;
    if (rd32_le(buf) != JDM_UDP_MAGIC)
        return false;

    uint8_t player = buf[4];
    if (player >= SCE_MOVE_MAX_PLAYERS)
        return false;

    uint8_t flags = buf[5];
    uint16_t buttons = rd16_le(buf + 6);
    uint8_t trigger = buf[8];

    float in_accel[3] = {
        rd_float_le(buf + 12),
        rd_float_le(buf + 16),
        rd_float_le(buf + 20),
    };
    float in_gyro[3] = {
        rd_float_le(buf + 24),
        rd_float_le(buf + 28),
        rd_float_le(buf + 32),
    };

    memset(out, 0, sizeof(*out));
    out->player_index = player;
    out->buttons = buttons;
    out->trigger = trigger ? trigger : ((buttons & SCE_MOVE_BUTTON_T) ? 255 : 0);
    apply_source_transform(flags, in_accel, in_gyro, accel_scale, out->accel_g, out->gyro_rads);
    return true;
}

/* ------------------------------------------------------------------ */
/* 2. Cemuhook / DSU (DSUS) Packet Parser                              */
/* ------------------------------------------------------------------ */

static bool parse_dsu(const uint8_t *buf, size_t len, float accel_scale, motion_sample_t *out) {
    if (len < 100)
        return false;
    if (rd32_le(buf) != DSU_MAGIC_SERVER)
        return false;

    uint16_t version = rd16_le(buf + 4);
    uint16_t payload_len = rd16_le(buf + 6);
    if (version != DSU_PROTOCOL_VERSION || (size_t)payload_len + 16 > len)
        return false;

    /* Validate CRC32 if sender populated it */
    uint32_t expected_crc = rd32_le(buf + 8);
    if (expected_crc != 0) {
        uint8_t tmp[128];
        size_t total = (size_t)payload_len + 16;
        if (total <= sizeof(tmp)) {
            memcpy(tmp, buf, total);
            wr32_le(tmp + 8, 0);
            if (protocol_crc32(tmp, total) != expected_crc)
                return false;
        }
    }

    uint32_t event_type = rd32_le(buf + 16);
    if (event_type != DSU_TYPE_CONTROLLER_DATA)
        return false;

    uint8_t slot = buf[20];
    uint8_t state = buf[21];
    uint8_t connected_flag = buf[31];
    if (slot >= SCE_MOVE_MAX_PLAYERS || state == 0 || connected_flag == 0)
        return false;

    uint8_t btn1 = buf[36];
    uint8_t btn2 = buf[37];
    uint8_t home = buf[38];
    uint8_t r2_analog = buf[54];
    uint8_t l2_analog = buf[55];

    uint16_t move_btns = 0;
    if (btn1 & 0x01) move_btns |= SCE_MOVE_BUTTON_SELECT;   /* Share / Minus */
    if (btn1 & 0x08) move_btns |= SCE_MOVE_BUTTON_START;    /* Options / Plus */
    if (btn2 & 0x80) move_btns |= SCE_MOVE_BUTTON_SQUARE;   /* Square / Y */
    if (btn2 & 0x40) move_btns |= SCE_MOVE_BUTTON_CROSS;    /* Cross / B */
    if (btn2 & 0x20) move_btns |= SCE_MOVE_BUTTON_CIRCLE;   /* Circle / A */
    if (btn2 & 0x10) move_btns |= SCE_MOVE_BUTTON_TRIANGLE; /* Triangle / X */
    if ((btn2 & 0x0C) || home) move_btns |= SCE_MOVE_BUTTON_MOVE; /* L1/R1/Home -> Move */
    if ((btn2 & 0x03) || r2_analog > 64 || l2_analog > 64)
        move_btns |= SCE_MOVE_BUTTON_T;                     /* L2/R2/ZL/ZR -> T */

    uint16_t trig = r2_analog > l2_analog ? r2_analog : l2_analog;
    if ((move_btns & SCE_MOVE_BUTTON_T) && trig == 0)
        trig = 255;

    float ax = sanitize_float(rd_float_le(buf + 76), 0.0f);
    float ay = sanitize_float(rd_float_le(buf + 80), 0.0f);
    float az = sanitize_float(rd_float_le(buf + 84), 1.0f);
    float pitch_deg = sanitize_float(rd_float_le(buf + 88), 0.0f);
    float yaw_deg   = sanitize_float(rd_float_le(buf + 92), 0.0f);
    float roll_deg  = sanitize_float(rd_float_le(buf + 96), 0.0f);

    if (accel_scale <= 0.0f)
        accel_scale = 1.0f;

    memset(out, 0, sizeof(*out));
    out->player_index = slot;
    out->buttons = move_btns;
    out->trigger = trig;
    out->accel_g[0] = ax * accel_scale;
    out->accel_g[1] = ay * accel_scale;
    out->accel_g[2] = az * accel_scale;
    out->gyro_rads[0] = pitch_deg * DEG_TO_RAD;
    out->gyro_rads[1] = yaw_deg * DEG_TO_RAD;
    out->gyro_rads[2] = roll_deg * DEG_TO_RAD;
    return true;
}

/* ------------------------------------------------------------------ */
/* 3. JSON Text Packet Parser (Smartphone Web App / Custom Scripts)    */
/* ------------------------------------------------------------------ */

static bool json_find_number(const char *json, const char *key, float *out_val) {
    char pattern[32];
    size_t klen = strlen(key);
    if (klen + 3 >= sizeof(pattern))
        return false;
    pattern[0] = '"';
    memcpy(pattern + 1, key, klen);
    pattern[klen + 1] = '"';
    pattern[klen + 2] = '\0';

    const char *p = strstr(json, pattern);
    if (p == NULL)
        return false;
    p += klen + 2;
    while (*p && (*p == ' ' || *p == '\t' || *p == ':'))
        p++;
    if (*p == '\0')
        return false;

    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p)
        return false;
    *out_val = sanitize_float(v, 0.0f);
    return true;
}

static bool parse_json_motion(const uint8_t *buf, size_t len, float accel_scale, motion_sample_t *out) {
    if (len < 2 || len > 1024)
        return false;

    /* Find the start of JSON object (either raw UDP JSON or body of HTTP POST) */
    const char *start = NULL;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '{') {
            start = (const char *)(buf + i);
            break;
        }
    }
    if (start == NULL)
        return false;

    size_t json_len = len - (size_t)(start - (const char *)buf);
    char copy[1025];
    if (json_len >= sizeof(copy))
        json_len = sizeof(copy) - 1;
    memcpy(copy, start, json_len);
    copy[json_len] = '\0';

    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    bool has_ax = json_find_number(copy, "ax", &ax);
    bool has_ay = json_find_number(copy, "ay", &ay);
    bool has_az = json_find_number(copy, "az", &az);
    if (!has_ax && !has_ay && !has_az)
        return false;

    float player_f = 0.0f, gx = 0.0f, gy = 0.0f, gz = 0.0f, btn_f = 0.0f, trig_f = 0.0f, mode_f = 3.0f;
    json_find_number(copy, "p", &player_f);
    json_find_number(copy, "gx", &gx);
    json_find_number(copy, "gy", &gy);
    json_find_number(copy, "gz", &gz);
    json_find_number(copy, "btn", &btn_f);
    json_find_number(copy, "trig", &trig_f);
    /* Default mode for JSON is JDM_SOURCE_PHONE (m/s^2 and deg/s) unless "mode":0 is passed */
    json_find_number(copy, "mode", &mode_f);

    int player = (int)player_f;
    if (player < 0 || player >= SCE_MOVE_MAX_PLAYERS)
        return false;

    float in_accel[3] = { ax, ay, az };
    float in_gyro[3]  = { gx, gy, gz };
    uint16_t btns = (uint16_t)((int)btn_f & 0xFFFF);
    uint16_t trig = (uint16_t)((int)trig_f & 0xFF);
    if ((btns & SCE_MOVE_BUTTON_T) && trig == 0)
        trig = 255;

    memset(out, 0, sizeof(*out));
    out->player_index = player;
    out->buttons = btns;
    out->trigger = trig;
    apply_source_transform((uint8_t)mode_f, in_accel, in_gyro, accel_scale, out->accel_g, out->gyro_rads);
    return true;
}

/* ------------------------------------------------------------------ */
/* Unified Dispatcher                                                  */
/* ------------------------------------------------------------------ */

bool protocol_parse_packet(const uint8_t *buf, size_t len, float accel_scale, motion_sample_t *out) {
    if (buf == NULL || out == NULL || len < 4)
        return false;

    uint32_t magic = rd32_le(buf);
    if (magic == JDM_UDP_MAGIC)
        return parse_jdm1(buf, len, accel_scale, out);
    if (magic == DSU_MAGIC_SERVER)
        return parse_dsu(buf, len, accel_scale, out);

    return parse_json_motion(buf, len, accel_scale, out);
}
