#!/usr/bin/env python3
"""
joycon_bridge.py - PS5JustDance Companion Bridge for Nintendo Switch Joy-Cons

Reads 1 to 4 Nintendo Switch Joy-Cons (Left or Right) over Bluetooth HID
(via `hid` / `hidapi`), enables the 60Hz 6-axis IMU (accelerometer + gyroscope),
and streams 36-byte JDM1 UDP packets to PS5JustDance running on the PS5.

Also maps the Joy-Con analog stick and buttons to PS4/PS5 UI navigation buttons
(D-Pad Up/Down/Left/Right, Cross, Circle, Options) so you can navigate the
Just Dance menus directly from the Joy-Con!

Requirements (for real Joy-Con):
    pip install hidapi

Usage:
    python3 tools/joycon_bridge.py --ps5-ip 192.168.1.100
    python3 tools/joycon_bridge.py --ps5-ip 192.168.1.100 --simulate
"""

import argparse
import math
import socket
import struct
import sys
import time

NINTENDO_VID = 0x057E
JOYCON_L_PID = 0x2006
JOYCON_R_PID = 0x2007

JDM_UDP_MAGIC = 0x314D444A  # "JDM1"
JDM_SOURCE_GENERIC  = 0x00
JDM_SOURCE_JOYCON_R = 0x01
JDM_SOURCE_JOYCON_L = 0x02

SCE_MOVE_BUTTON_SELECT   = 1 << 0
SCE_MOVE_BUTTON_T        = 1 << 1
SCE_MOVE_BUTTON_MOVE     = 1 << 2
SCE_MOVE_BUTTON_START    = 1 << 3
SCE_MOVE_BUTTON_TRIANGLE = 1 << 4
SCE_MOVE_BUTTON_CIRCLE   = 1 << 5
SCE_MOVE_BUTTON_CROSS    = 1 << 6
SCE_MOVE_BUTTON_SQUARE   = 1 << 7

SCE_PAD_BUTTON_SELECT    = 1 << 0
SCE_PAD_BUTTON_OPTIONS   = 1 << 3
SCE_PAD_BUTTON_UP        = 1 << 4
SCE_PAD_BUTTON_RIGHT     = 1 << 5
SCE_PAD_BUTTON_DOWN      = 1 << 6
SCE_PAD_BUTTON_LEFT      = 1 << 7
SCE_PAD_BUTTON_L2        = 1 << 8
SCE_PAD_BUTTON_R2        = 1 << 9
SCE_PAD_BUTTON_L1        = 1 << 10
SCE_PAD_BUTTON_R1        = 1 << 11
SCE_PAD_BUTTON_TRIANGLE  = 1 << 12
SCE_PAD_BUTTON_CIRCLE    = 1 << 13
SCE_PAD_BUTTON_CROSS     = 1 << 14
SCE_PAD_BUTTON_SQUARE    = 1 << 15

# Joy-Con IMU scale factors (standard ±8G, ±2000 dps)
ACCEL_LSB_TO_G   = 0.000244
GYRO_LSB_TO_RADS = 0.06103 * (math.pi / 180.0)


def pack_jdm1(player_index, source_flags, buttons, trigger, pad_buttons, accel, gyro):
    return struct.pack(
        "<IBBHBBHffffff",
        JDM_UDP_MAGIC,
        player_index & 0x03,
        source_flags & 0xFF,
        buttons & 0xFFFF,
        trigger & 0xFF,
        0,
        pad_buttons & 0xFFFF,
        float(accel[0]), float(accel[1]), float(accel[2]),
        float(gyro[0]), float(gyro[1]), float(gyro[2]),
    )


def send_subcommand(dev, packet_num, subcmd, data=b""):
    buf = bytearray(49)
    buf[0] = 0x01
    buf[1] = packet_num & 0x0F
    # neutral rumble bytes
    buf[2:10] = b"\x00\x01\x40\x40\x00\x01\x40\x40"
    buf[10] = subcmd
    buf[11:11 + len(data)] = data
    dev.write(buf)


def init_joycon(dev, player_slot):
    """Enables IMU (0x40 = 1) and sets standard full 60Hz input report mode (0x03 = 0x30)."""
    send_subcommand(dev, 0, 0x40, b"\x01")
    time.sleep(0.05)
    send_subcommand(dev, 1, 0x03, b"\x30")
    time.sleep(0.05)
    # Set player LED (1 << player_slot)
    send_subcommand(dev, 2, 0x30, bytes([1 << (player_slot & 3)]))
    dev.set_nonblocking(True)


def decode_stick_dpad(stick_bytes):
    """Decodes 3-byte Joy-Con analog stick data into D-Pad bitmask."""
    if len(stick_bytes) < 3:
        return 0
    sx = stick_bytes[0] | ((stick_bytes[1] & 0x0F) << 8)
    sy = (stick_bytes[1] >> 4) | (stick_bytes[2] << 4)
    # Center is ~2048, range ~600..3400
    pad = 0
    if sx < 1300:
        pad |= SCE_PAD_BUTTON_LEFT
    elif sx > 2800:
        pad |= SCE_PAD_BUTTON_RIGHT
    if sy < 1300:
        pad |= SCE_PAD_BUTTON_DOWN
    elif sy > 2800:
        pad |= SCE_PAD_BUTTON_UP
    return pad


def parse_joycon_report(report, is_left):
    """Parses a 0x30 standard Joy-Con input report into (buttons, trigger, pad_buttons, accel, gyro)."""
    if len(report) < 25 or report[0] != 0x30:
        return None

    btn_right = report[3]
    btn_shared = report[4]
    btn_left = report[5]

    move_btns = 0
    pad_btns = 0
    trigger = 0

    if not is_left:
        # Right Joy-Con button mapping -> PS Move + PS4/PS5 UI Pad
        pad_btns |= decode_stick_dpad(report[9:12])
        if btn_right & 0x08:  # A -> Cross (Confirm) + Move
            move_btns |= SCE_MOVE_BUTTON_CROSS | SCE_MOVE_BUTTON_MOVE
            pad_btns |= SCE_PAD_BUTTON_CROSS
        if btn_right & 0x04:  # B -> Circle (Back)
            move_btns |= SCE_MOVE_BUTTON_CIRCLE
            pad_btns |= SCE_PAD_BUTTON_CIRCLE
        if btn_right & 0x02:  # X -> Triangle
            move_btns |= SCE_MOVE_BUTTON_TRIANGLE
            pad_btns |= SCE_PAD_BUTTON_TRIANGLE
        if btn_right & 0x01:  # Y -> Square
            move_btns |= SCE_MOVE_BUTTON_SQUARE
            pad_btns |= SCE_PAD_BUTTON_SQUARE
        if (btn_right & 0x40) or (btn_shared & 0x10):  # R or Home -> Move + R1
            move_btns |= SCE_MOVE_BUTTON_MOVE
            pad_btns |= SCE_PAD_BUTTON_R1
        if btn_right & 0x80:  # ZR -> T trigger + R2
            move_btns |= SCE_MOVE_BUTTON_T | SCE_MOVE_BUTTON_MOVE
            pad_btns |= SCE_PAD_BUTTON_R2
            trigger = 255
        if btn_shared & 0x02:  # Plus -> Start / Options
            move_btns |= SCE_MOVE_BUTTON_START
            pad_btns |= SCE_PAD_BUTTON_OPTIONS
    else:
        # Left Joy-Con button mapping -> PS Move + PS4/PS5 UI Pad
        pad_btns |= decode_stick_dpad(report[6:9])
        if btn_left & 0x01:  # Down -> Cross / Down
            move_btns |= SCE_MOVE_BUTTON_CROSS
            pad_btns |= SCE_PAD_BUTTON_DOWN
        if btn_left & 0x08:  # Left -> Circle / Left
            move_btns |= SCE_MOVE_BUTTON_CIRCLE
            pad_btns |= SCE_PAD_BUTTON_LEFT
        if btn_left & 0x02:  # Up -> Triangle / Up
            move_btns |= SCE_MOVE_BUTTON_TRIANGLE
            pad_btns |= SCE_PAD_BUTTON_UP
        if btn_left & 0x04:  # Right -> Square / Right
            move_btns |= SCE_MOVE_BUTTON_SQUARE
            pad_btns |= SCE_PAD_BUTTON_RIGHT
        if (btn_left & 0x40) or (btn_shared & 0x20):  # L or Capture -> Move + Cross
            move_btns |= SCE_MOVE_BUTTON_MOVE | SCE_MOVE_BUTTON_CROSS
            pad_btns |= SCE_PAD_BUTTON_CROSS
        if btn_left & 0x80:  # ZL -> T trigger + R2
            move_btns |= SCE_MOVE_BUTTON_T | SCE_MOVE_BUTTON_MOVE
            pad_btns |= SCE_PAD_BUTTON_R2
            trigger = 255
        if btn_shared & 0x01:  # Minus -> Start / Options
            move_btns |= SCE_MOVE_BUTTON_START
            pad_btns |= SCE_PAD_BUTTON_OPTIONS

    # 3 IMU samples per packet at offsets 13, 25, 37; average available samples
    samples = 0
    ax_sum = ay_sum = az_sum = 0.0
    gx_sum = gy_sum = gz_sum = 0.0
    for off in (13, 25, 37):
        if len(report) >= off + 12:
            ax, ay, az, gx, gy, gz = struct.unpack_from("<hhhhhh", bytes(report), off)
            ax_sum += ax * ACCEL_LSB_TO_G
            ay_sum += ay * ACCEL_LSB_TO_G
            az_sum += az * ACCEL_LSB_TO_G
            gx_sum += gx * GYRO_LSB_TO_RADS
            gy_sum += gy * GYRO_LSB_TO_RADS
            gz_sum += gz * GYRO_LSB_TO_RADS
            samples += 1

    if samples == 0:
        return None

    accel = (ax_sum / samples, ay_sum / samples, az_sum / samples)
    gyro = (gx_sum / samples, gy_sum / samples, gz_sum / samples)
    return move_btns, trigger, pad_btns, accel, gyro


def run_simulation(sock, target):
    print(f"[Simulate] Streaming synthetic dance motion to {target[0]}:{target[1]} (Ctrl+C to stop)...")
    t0 = time.time()
    while True:
        t = time.time() - t0
        # Rhythmic 120 BPM arm swing
        omega = 2.0 * math.pi * 2.0
        ax = 1.8 * math.sin(omega * t)
        ay = 1.4 * math.cos(omega * t)
        az = 1.0 + 0.9 * math.sin(2.0 * omega * t)
        gx = 3.5 * math.cos(omega * t)
        gy = -2.8 * math.sin(omega * t)
        gz = 1.8 * math.sin(omega * t)

        pkt = pack_jdm1(0, JDM_SOURCE_GENERIC, 0, 0, 0, (ax, ay, az), (gx, gy, gz))
        sock.sendto(pkt, target)
        time.sleep(0.01)


def main():
    parser = argparse.ArgumentParser(description="Switch Joy-Con to PS5JustDance UDP Bridge")
    parser.add_argument("--ps5-ip", required=True, help="IP address of the jailbroken PS5")
    parser.add_argument("--port", type=int, default=9052, help="PS5JustDance UDP port (default: 9052)")
    parser.add_argument("--simulate", action="store_true", help="Send simulated dance motion without hardware")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.ps5_ip, args.port)

    if args.simulate:
        try:
            run_simulation(sock, target)
        except KeyboardInterrupt:
            print("\nStopped.")
        return

    try:
        import hid
    except ImportError:
        print("Error: python 'hidapi' module is not installed. Run: pip install hidapi", file=sys.stderr)
        sys.exit(1)

    devices = []
    for d in hid.enumerate(NINTENDO_VID, 0):
        pid = d.get("product_id")
        if pid in (JOYCON_L_PID, JOYCON_R_PID) and len(devices) < 4:
            dev = hid.device()
            dev.open_path(d["path"])
            slot = len(devices)
            is_left = (pid == JOYCON_L_PID)
            init_joycon(dev, slot)
            devices.append((slot, is_left, dev))
            side = "Left" if is_left else "Right"
            print(f"[Joy-Con] Connected Player {slot + 1}: Joy-Con ({side})")

    if not devices:
        print("No paired Switch Joy-Cons found via Bluetooth HID. Pair your Joy-Con first or use --simulate.")
        sys.exit(1)

    print(f"Streaming {len(devices)} Joy-Con(s) to {args.ps5_ip}:{args.port} (Ctrl+C to stop)...")
    try:
        while True:
            any_read = False
            for slot, is_left, dev in devices:
                report = dev.read(64)
                if report:
                    any_read = True
                    parsed = parse_joycon_report(report, is_left)
                    if parsed is not None:
                        btns, trig, pad_btns, accel, gyro = parsed
                        src_flag = JDM_SOURCE_JOYCON_L if is_left else JDM_SOURCE_JOYCON_R
                        pkt = pack_jdm1(slot, src_flag, btns, trig, pad_btns, accel, gyro)
                        sock.sendto(pkt, target)
            if not any_read:
                time.sleep(0.002)
    except KeyboardInterrupt:
        print("\nStopping Joy-Con bridge...")


if __name__ == "__main__":
    main()
