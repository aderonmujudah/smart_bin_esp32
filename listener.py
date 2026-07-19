#!/usr/bin/env python3
"""
Serial listener for the Smart Bin ESP32.

Opens the ESP32's serial port at 115200 baud (the sketch's Serial.begin()
rate) and prints every line it sends -- WiFi connection status, sensor
readings, JSON payloads, HTTP response codes, etc. Reconnects automatically
if the port drops (e.g. during a re-flash).

Usage:
    python serial_listener.py [PORT]

    PORT defaults to COM5. On Linux/Mac this would look like /dev/ttyUSB0.

Requires: pip install pyserial
"""
import sys
import time

import serial

BAUD_RATE = 115200
DEFAULT_PORT = "COM5"

# Windows consoles default to cp1252, which chokes on stray non-ASCII bytes
# (e.g. garbage during the ESP32's boot/reset). Reconfigure to UTF-8 and
# replace anything undecodable instead of crashing the listener.
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def listen(port: str) -> None:
    while True:
        try:
            with serial.Serial(port, BAUD_RATE, timeout=1) as ser:
                # Discard whatever was already buffered by the OS/driver from
                # before this connection -- otherwise stale bytes get drained
                # instantly and look like a garbled flood of "live" data.
                time.sleep(0.3)
                ser.reset_input_buffer()
                print(f"[listener] connected to {port} at {BAUD_RATE} baud")
                while True:
                    line = ser.readline()
                    if not line:
                        continue
                    text = line.decode("utf-8", errors="replace").rstrip("\r\n")
                    if text:
                        print(text)
        except serial.SerialException as exc:
            print(f"[listener] {port} unavailable ({exc}); retrying in 2s...")
            time.sleep(2)
        except KeyboardInterrupt:
            print("\n[listener] stopped.")
            return


if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PORT
    listen(port)
