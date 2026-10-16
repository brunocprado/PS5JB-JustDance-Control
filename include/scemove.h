/*
    scemove.h - PS5JustDance
    PlayStation 4 libSceMove data structures, constants, and return codes
    (SysV x86-64 ABI).

    Licensed under the GNU Lesser General Public License version 2.1, or later.
*/

#ifndef PS5JD_SCEMOVE_H
#define PS5JD_SCEMOVE_H

#include <stdint.h>

#define SCE_MOVE_MAX_PLAYERS   4
#define SCE_MOVE_MAX_STATE_NUM 32

/* Return codes and error constants from libSceMove */
#define SCE_MOVE_OK                                 0
#define SCE_MOVE_RETURN_CODE_NO_CONTROLLER_CONNECTED 1
#define SCE_MOVE_ERROR_NOT_INIT                     ((int32_t)0x80EE0001)
#define SCE_MOVE_ERROR_ALREADY_INIT                 ((int32_t)0x80EE0002)
#define SCE_MOVE_ERROR_INVALID_ARG                  ((int32_t)0x80EE0003)
#define SCE_MOVE_ERROR_INVALID_HANDLE               ((int32_t)0x80EE0004)
#define SCE_MOVE_ERROR_MAX_CONTROLLERS_EXCEEDED     ((int32_t)0x80EE0005)
#define SCE_MOVE_ERROR_INVALID_PORT                 ((int32_t)0x80EE0006)

/* Digital button bitmasks (SceMoveButtonData::digital_buttons) */
#define SCE_MOVE_BUTTON_SELECT   (1u << 0) /* 0x0001 */
#define SCE_MOVE_BUTTON_T        (1u << 1) /* 0x0002 */
#define SCE_MOVE_BUTTON_MOVE     (1u << 2) /* 0x0004 */
#define SCE_MOVE_BUTTON_START    (1u << 3) /* 0x0008 */
#define SCE_MOVE_BUTTON_TRIANGLE (1u << 4) /* 0x0010 */
#define SCE_MOVE_BUTTON_CIRCLE   (1u << 5) /* 0x0020 */
#define SCE_MOVE_BUTTON_CROSS    (1u << 6) /* 0x0040 */
#define SCE_MOVE_BUTTON_SQUARE   (1u << 7) /* 0x0080 */

/* IEEE-754 bit representations used by the freestanding stub (-mno-sse) */
#define IEEE754_ZERO_BITS        0x00000000u /* 0.0f  */
#define IEEE754_ONE_BITS         0x3F800000u /* 1.0f  (1G gravity) */
#define IEEE754_SPHERE_RAD_BITS  0x41B40000u /* 22.5f (mm sphere radius) */
#define IEEE754_ROOM_TEMP_BITS   0x41C80000u /* 25.0f (Celsius) */

typedef struct SceMoveDeviceInfo {
    uint32_t sphere_radius_bits;           /* float: 22.5f */
    uint32_t accel_to_sphere_offset_bits[3]; /* float[3]: {0.0f, 0.0f, 0.0f} */
} SceMoveDeviceInfo;

typedef struct SceMoveButtonData {
    uint16_t digital_buttons;
    uint16_t analog_t;
} SceMoveButtonData;

typedef struct SceMoveExtensionPortData {
    uint16_t status;
    uint16_t digital1;
    uint16_t digital2;
    uint16_t analog_right_x;
    uint16_t analog_right_y;
    uint16_t analog_left_x;
    uint16_t analog_left_y;
    uint8_t custom[5];
    uint8_t _pad;
} SceMoveExtensionPortData;

/*
    SceMoveData (64 bytes on PS4 x86-64 ABI):
      offset  0: float accelerometer[3]  (in G, 1.0f == 9.80665 m/s^2)
      offset 12: float gyro[3]           (in rad/s)
      offset 24: SceMoveButtonData pad   (4 bytes)
      offset 28: SceMoveExtensionPortData ext (20 bytes)
      offset 48: uint64_t timestamp      (microseconds)
      offset 56: int32_t counter         (monotonic sample counter)
      offset 60: float temperature
*/
typedef struct SceMoveData {
    uint32_t accel_bits[3];
    uint32_t gyro_bits[3];
    SceMoveButtonData pad;
    SceMoveExtensionPortData ext;
    uint64_t timestamp;
    int32_t counter;
    uint32_t temperature_bits;
} SceMoveData;

#endif
