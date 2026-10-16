/*
    protocol.h - PS5JustDance
    Motion input protocols supported by the PS5 daemon:
      1. JDM1 binary UDP packet (direct Joy-Con / Phone bridge, port 9052)
      2. Cemuhook / DSU (DualShock UDP) protocol (BetterJoy, joycond-cemuhook,
         DSUController Android/iOS, MotionSource)
      3. JSON text packet over UDP or HTTP POST (built-in Phone Web Controller)

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_PROTOCOL_H
#define PS5JD_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "scemove.h"

#define JDM_UDP_MAGIC       0x314D444Au /* "JDM1" little-endian */
#define JDM_DEFAULT_PORT    9052
#define JDM_WEB_PORT        8080
#define DSU_DEFAULT_PORT    26760

/* Device orientation / source flags in jdm_packet_t::source_flags */
#define JDM_SOURCE_GENERIC  0x00 /* Already in PS Move frame (accel in G, gyro in rad/s) */
#define JDM_SOURCE_JOYCON_R 0x01 /* Switch Right Joy-Con raw/standard frame */
#define JDM_SOURCE_JOYCON_L 0x02 /* Switch Left Joy-Con raw/standard frame */
#define JDM_SOURCE_PHONE    0x03 /* Smartphone (accel in m/s^2, gyro in deg/s) */

/*
    Native 32-byte binary UDP packet ("JDM1").
    Little-endian layout:
      offset  0: uint32_t magic        (0x314D444A = "JDM1")
      offset  4: uint8_t  player_index (0..3)
      offset  5: uint8_t  source_flags (JDM_SOURCE_*)
      offset  6: uint16_t buttons      (SCE_MOVE_BUTTON_* mask)
      offset  8: uint8_t  trigger      (0..255 analog T trigger)
      offset  9: uint8_t  reserved[3]
      offset 12: float    accel[3]     (X, Y, Z)
      offset 24: float    gyro[3]      (X, Y, Z)
    Total: 36 bytes.
*/
typedef struct __attribute__((packed)) jdm_packet {
    uint32_t magic;
    uint8_t player_index;
    uint8_t source_flags;
    uint16_t buttons;
    uint8_t trigger;
    uint8_t reserved[3];
    float accel[3];
    float gyro[3];
} jdm_packet_t;

/* Normalized motion sample ready to be written into stub_controller_state_t */
typedef struct motion_sample {
    int player_index;       /* 0..3 */
    float accel_g[3];       /* PS Move frame: X, Y, Z in G (1.0 = 9.80665 m/s^2) */
    float gyro_rads[3];     /* PS Move frame: X, Y, Z in rad/s */
    uint16_t buttons;       /* SCE_MOVE_BUTTON_* */
    uint16_t trigger;       /* 0..255 */
} motion_sample_t;

/* Cemuhook / DSU protocol constants */
#define DSU_MAGIC_SERVER          0x53555344u /* "DSUS" little-endian */
#define DSU_MAGIC_CLIENT          0x43555344u /* "DSUC" little-endian */
#define DSU_PROTOCOL_VERSION      1001
#define DSU_TYPE_VERSION_INFO     0x00100000u
#define DSU_TYPE_CONTROLLER_INFO  0x00100001u
#define DSU_TYPE_CONTROLLER_DATA  0x00100002u

/* Computes standard IEEE 802.3 CRC32 (used by DSU packets). */
uint32_t protocol_crc32(const uint8_t *data, size_t len);

/* Builds a 24-byte DSUC controller data subscription request packet for `slot` (0..3) or all slots (-1). */
size_t protocol_build_dsu_request(uint8_t *out_buf, size_t max_len, uint32_t client_id, int slot);

/*
    Parses any supported incoming packet (JDM1 binary, DSUS Cemuhook, or JSON text)
    and populates `out` in PS Move coordinates (G and rad/s).
    Returns true if a valid controller motion sample was parsed.
*/
bool protocol_parse_packet(const uint8_t *buf, size_t len, float accel_scale, motion_sample_t *out);

/* Converts float to its raw IEEE-754 uint32_t representation. */
static inline uint32_t float_to_bits(float v) {
    uint32_t bits;
    __builtin_memcpy(&bits, &v, sizeof(bits));
    return bits;
}

static inline float bits_to_float(uint32_t bits) {
    float v;
    __builtin_memcpy(&v, &bits, sizeof(v));
    return v;
}

#endif
