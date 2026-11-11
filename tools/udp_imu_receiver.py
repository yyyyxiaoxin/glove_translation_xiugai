#!/usr/bin/env python3
"""Receive and display ESP32-S3 glove UDP test frames on the LAN."""

import argparse
import socket


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32-S3 glove UDP receiver")
    parser.add_argument("--port", type=int, default=9101,
                        help="left=9101, right=9102 (default: 9101)")
    parser.add_argument("--raw", action="store_true",
                        help="print the complete CSV packet")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", args.port))
    print(f"Listening on UDP 0.0.0.0:{args.port} ...")

    while True:
        payload, peer = sock.recvfrom(2048)
        text = payload.decode("utf-8", errors="replace").strip()
        if args.raw or not text.startswith("[DATA]"):
            print(f"{peer[0]}:{peer[1]} {text}")
            continue

        fields = text[len("[DATA]"):].split(",")
        if len(fields) != 72:
            print(f"{peer[0]} invalid field count={len(fields)}: {text}")
            continue

        timestamp, hand, sequence = fields[:3]
        ax, ay, az, gx, gy, gz = fields[3:9]
        print(
            f"from={peer[0]} hand={hand} seq={sequence} t={timestamp}ms "
            f"MPU_CH0 acc=({ax},{ay},{az}) gyro=({gx},{gy},{gz})"
        )


if __name__ == "__main__":
    main()
