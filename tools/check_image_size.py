#!/usr/bin/env python3
"""Fail the build when the firmware image is larger than System Update accepts.

    python3 tools/check_image_size.py build/picodeck.bin --header src/os/ota_verify.h

applyUpdate and the boot both refuse an image over OTA_MAX_SIZE (src/os/ota_verify.h),
so a build past it would flash by UF2 and then never update over the air. The cap is
read from the header, so there is one number. Tests: tests/unit/test_check_image_size.py.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

DEFINE_RE = re.compile(r"^\s*#\s*define\s+OTA_MAX_SIZE\s+\(?([0-9uU\s*()]+?)\)?\s*(//.*)?$", re.M)


def read_cap(header: Path) -> int:
    m = DEFINE_RE.search(header.read_text())
    if not m:
        raise SystemExit(f"check_image_size: no OTA_MAX_SIZE in {header}")
    expr = m.group(1).replace("u", "").replace("U", "")
    if not re.fullmatch(r"[0-9\s*()]+", expr):
        raise SystemExit(f"check_image_size: cannot read OTA_MAX_SIZE ({m.group(1)!r})")
    value = 1
    for factor in expr.replace("(", " ").replace(")", " ").split("*"):
        value *= int(factor)
    return value


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", type=Path)
    ap.add_argument("--header", type=Path, required=True)
    args = ap.parse_args(argv)
    cap = read_cap(args.header)
    size = args.image.stat().st_size
    if size > cap:
        print(f"image {size} bytes exceeds OTA_MAX_SIZE {cap}: System Update would refuse it",
              file=sys.stderr)
        return 1
    print(f"image {size} of {cap} bytes ({size * 100 // cap}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
