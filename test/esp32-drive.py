#!/usr/bin/env python3
"""Drive the wsh supervisor over UART: reset the board, capture boot, then run a
scripted command sequence. Prints everything received with elapsed timestamps.

Usage: esp32-drive.py [cmd ...]   (each arg is one wsh line; default = repro)
"""
import sys
import time
import threading
import serial

PORT = "/dev/ttyUSB0"
BAUD = 115200

DEFAULT = [
    "@2 status",
    "@1 create blink",
    '@1 set_config blink {"drivers":[{"name":"gpio"}]}',
    "@1 start blink",
    "@2 status",
    "@1 create hello",
    '@1 set_config hello {"image":"hello"}',
    "@1 start hello",
    "@2 status",
]


def main():
    cmds = sys.argv[1:] or DEFAULT
    ser = serial.Serial(PORT, BAUD, timeout=0.1)

    t0 = time.time()
    stop = threading.Event()

    def reader():
        buf = b""
        while not stop.is_set():
            data = ser.read(4096)
            if data:
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    print("%7.2f | %s" % (time.time() - t0,
                          line.decode("utf-8", "replace").rstrip("\r")))
                    sys.stdout.flush()

    th = threading.Thread(target=reader, daemon=True)
    th.start()

    # Reset the ESP32: classic auto-reset (EN=RTS, IO0=DTR). Drive EN low then
    # release, with IO0 held high so it runs (not bootloader).
    ser.setDTR(False)   # IO0 high
    ser.setRTS(True)    # EN low (reset asserted)
    time.sleep(0.1)
    ser.setRTS(False)   # EN high (release)
    time.sleep(0.05)

    # Let it boot before driving.
    time.sleep(4.0)

    for c in cmds:
        delay = 1.5
        if c.startswith("@"):
            tok, c = c.split(" ", 1)
            delay = float(tok[1:])
        print("%7.2f | >>> %s" % (time.time() - t0, c))
        sys.stdout.flush()
        ser.write((c + "\n").encode())
        ser.flush()
        time.sleep(delay)

    time.sleep(1.0)
    stop.set()
    th.join(timeout=1)
    ser.close()


if __name__ == "__main__":
    main()
