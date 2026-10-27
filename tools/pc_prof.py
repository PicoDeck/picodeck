#!/usr/bin/env python3
"""Profile the running app on a PicoDeck device (the `prof` dev command).

The firmware samples the interrupted PC and LR of the running app every
`--period` microseconds (src/os/pc_prof.c); this script starts it, waits,
dumps the samples and attributes them to functions of the build's ELF:
self time per function, with the three commonest callers (from LR, which is
the caller while the sampled function is a leaf or has not saved LR yet).

Usage:
    python3 tools/pc_prof.py                         # sample 5 s of whatever runs
    python3 tools/pc_prof.py --launch hello -s 8     # launch an app first
    python3 tools/pc_prof.py --launch .test/bench    # a hidden app
    python3 tools/pc_prof.py --save run.txt          # keep the raw dump
    python3 tools/pc_prof.py --report run.txt        # re-symbolise a saved dump

Samples are flash PCs, so --elf must be the build that is on the device
(default build/picodeck.elf). Close anything else holding the serial port
(the MCP log capture: stop_log_capture) first. Each sample is two words of
one umm block in PSRAM, kept until `prof free` or a reboot.

Requires: pyserial; arm-none-eabi-nm on PATH.
"""

import argparse
import bisect
import collections
import glob
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def find_device():
    for pattern in ["/dev/serial/by-id/*PicoDeck*", "/dev/ttyACM*",
                    "/dev/tty.usbmodem*"]:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    return None


def command(ser, line, wait=0.3):
    ser.write((line + "\n").encode())
    ser.flush()
    time.sleep(wait)
    return ser.read(ser.in_waiting or 1).decode(errors="replace")


def dump(ser, timeout=60.0):
    ser.reset_input_buffer()
    ser.write(b"prof dump\n")
    ser.flush()
    buf = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk = ser.read(65536)
        if chunk:
            buf += chunk
            if b"[DEV] PROF end" in buf:
                break
    return buf.decode(errors="replace")


def symbols(elf):
    out = subprocess.run(["arm-none-eabi-nm", "-n", "-S", "--defined-only", elf],
                         capture_output=True, text=True, check=True).stdout
    syms = []
    for ln in out.splitlines():
        p = ln.split()
        if len(p) == 4 and p[2] in "tTwW":
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
    syms.sort()
    return syms


def namer(syms):
    starts = [a for a, _, _ in syms]

    def name(addr):
        addr &= ~1
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0:
            a, size, n = syms[i]
            if addr < a + max(size, 2):
                return n
        if 0x11000000 <= addr < 0x12000000:
            return "(QMI PSRAM)"  # native app code, or a stale LR
        return "(0x%08x)" % addr
    return name


def report(text, elf, top):
    lines = [l for l in text.splitlines() if l.startswith("%")]
    pairs = re.findall(r"([0-9a-f]{8}):([0-9a-f]{8})", "\n".join(lines))
    hdr = re.search(r"PROF dump samples=(\d+) skipped=(\d+)", text)
    if hdr:
        print("samples=%s skipped=%s (no app frame on the PSP)" % hdr.groups())
    if not pairs:
        print("no samples")
        return
    name = namer(symbols(elf))
    own = collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    for pc, lr in pairs:
        f = name(int(pc, 16))
        own[f] += 1
        callers[f][name(int(lr, 16))] += 1
    n = len(pairs)
    for f, c in own.most_common(top):
        via = ", ".join("%s %d" % kv for kv in callers[f].most_common(3))
        print("%6.2f%% %7d  %-36s <- %s" % (100.0 * c / n, c, f, via))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-d", "--device", help="serial device (default: auto-detect)")
    ap.add_argument("--elf", default="build/picodeck.elf",
                    help="the ELF of the firmware on the device")
    ap.add_argument("-s", "--seconds", type=float, default=5.0)
    ap.add_argument("-p", "--period", type=int, default=200,
                    help="sample period in microseconds (>= 50)")
    ap.add_argument("--max-samples", type=int, default=32768)
    ap.add_argument("--launch", help="app to launch first (dev `launch` name)")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--save", help="write the raw dump here")
    ap.add_argument("--report", help="symbolise a saved dump instead of sampling")
    args = ap.parse_args()

    if args.report:
        report(open(args.report).read(), args.elf, args.top)
        return

    port = args.device or find_device()
    if not port:
        sys.exit("No PicoDeck serial device found (use -d)")
    ser = serial.Serial(port, 115200, timeout=0.5)
    ser.reset_input_buffer()
    if args.launch:
        print(command(ser, "launch " + args.launch, 1.0).strip())
        time.sleep(1.0)
    print(command(ser, "prof start %d %d" % (args.period, args.max_samples)).strip())
    time.sleep(args.seconds)
    text = dump(ser)
    if args.save:
        with open(args.save, "w") as f:
            f.write(text)
    report(text, args.elf, args.top)


if __name__ == "__main__":
    main()
