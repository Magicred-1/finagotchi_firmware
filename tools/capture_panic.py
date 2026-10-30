#!/usr/bin/env python3
"""Capture an ESP32 panic/crash dump from the UART-bridge port.

The app's Serial prints go to the native USB port (ARDUINO_USB_CDC_ON_BOOT=1),
but the ROM/panic handler always writes to UART0 — which the USB-UART bridge
port carries. So a crash on the breadboard is capturable even without the
native port connected.

Usage:
    python3 tools/capture_panic.py            # listen 3 min on the default port
    python3 tools/capture_panic.py /dev/cu.usbmodemXXXX 300

Reproduce the crash while it listens, then send the printed section after
"=== CAPTURE ===" (saved to /tmp/finagotchi_panic.txt).
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem5B790726671"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 180

s = serial.Serial(port, 115200, timeout=0.2)
time.sleep(0.3)
s.reset_input_buffer()
print("listening on %s for %d s — reproduce the crash now..." % (port, seconds))
buf = b""
t0 = time.time()
while time.time() - t0 < seconds:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
s.close()

text = buf.decode("utf-8", "replace")
out = "/tmp/finagotchi_panic.txt"
open(out, "w").write(text)
boots = text.count("ESP-ROM")
crashes = [l for l in text.splitlines()
           if any(k in l for k in ("Guru", "panic", "Backtrace", "abort", "assert"))]
print("captured %d bytes -> %s" % (len(buf), out))
print("boot banners: %d, crash lines: %d" % (boots, len(crashes)))
print("=== CAPTURE (tail) ===")
print(text[-2000:] if text else "(no output — the port may be wrong or nothing crashed)")
