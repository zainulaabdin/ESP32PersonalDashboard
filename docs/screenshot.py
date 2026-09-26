"""Capture screenshots of the ESP32Bus TFT over USB serial.

The firmware answers the serial command "shot" with "SHOT <w> <h>\\n"
followed by the raw frame (little-endian RGB565). "tab <n>" switches tabs
(0 Profile, 1 Today, 2 Bus, 3 Ask, 4 Settings), "weather" opens the
weather screen and "mode day|night|auto" sets the screen mode. The script
captures every screen in day and night, then puts the original mode back.

Usage (close any serial monitor first):
    pip install pyserial pillow
    python docs/screenshot.py                 # -> docs/screenshots/day|night/
    python docs/screenshot.py --modes night --only bus
"""
import argparse
import os
import time

import serial
from PIL import Image

SCREENS = [
    ("weather", "weather"),
    ("profile", "tab 0"),
    ("today", "tab 1"),
    ("bus", "tab 2"),
    ("ask", "tab 3"),
    ("settings", "tab 4"),
]


def reply(port, cmd, prefix, timeout=10):
    """Send cmd, return the first line starting with prefix."""
    port.reset_input_buffer()
    port.write((cmd + "\n").encode())
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = port.readline().decode(errors="ignore").strip()
        if line.startswith(prefix):
            return line
    return None


def grab(port):
    line = reply(port, "shot", "SHOT ")
    if not line:
        raise RuntimeError("no SHOT reply - is the firmware up to date?")
    w, h = map(int, line.split()[1:3])
    raw = port.read(w * h * 2)
    if len(raw) != w * h * 2:
        raise RuntimeError("short frame: %d bytes" % len(raw))
    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)
        px[i % w, i // w] = ((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM8")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "screenshots"))
    ap.add_argument("--modes", default="day,night")
    ap.add_argument("--only", help="capture one screen: " + ", ".join(n for n, _ in SCREENS))
    ap.add_argument("--settle", type=float, default=3.0, help="seconds to wait after switching screens")
    args = ap.parse_args()

    with serial.Serial(args.port, 115200, timeout=5) as port:
        time.sleep(0.5)
        original = reply(port, "mode", "MODE ")
        try:
            for mode in args.modes.split(","):
                reply(port, "mode " + mode, "MODE ")
                folder = os.path.join(args.out, mode)
                os.makedirs(folder, exist_ok=True)
                for name, cmd in SCREENS:
                    if args.only and name != args.only:
                        continue
                    port.write((cmd + "\n").encode())
                    time.sleep(args.settle)
                    path = os.path.join(folder, name + ".png")
                    grab(port).save(path)
                    print("saved", path)
        finally:
            if original:
                reply(port, "mode " + original.split()[1], "MODE ")


if __name__ == "__main__":
    main()
