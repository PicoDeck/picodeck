#!/usr/bin/env python3
"""Guard the three copies of the native app ABI against drift.

PicoCalcAPI and its sub-tables exist three times:

  1. src/os/os.h                       the firmware's (and the simulator's) view
  2. sdk/native/os.h (+ terminal.h)    what native apps compile against
  3. simulator/unicorn_trampolines.c   the table the Unicorn runner builds

A mismatch shifts every field after it; `version`, the tail field, then reads
as garbage (this has happened). The checks:

  - every picocalc_*_t, pczip_stat_t and PicoCalcAPI declaration is the same
    in (1) and (2), member by member (comments and spacing ignored);
  - g_api.version in src/main.c equals the version the trampolines write, and
    they write it at offsetof(PicoCalcAPI, version) with api_struct_size ==
    sizeof(PicoCalcAPI), and a table pointer at every other offset (tables
    appended after `version` included);
  - with --cc (an ARM cross compiler, e.g. arm-none-eabi-gcc): both headers
    are compiled and the real sizeof() of every struct and offsetof(version)
    are compared (read back from symbol sizes with the matching nm).

The per-table trampoline slot counts are checked at run time by the E2E
probe app (tests/e2e/test_native.py::test_api_layout_matches_os_h).

Exit status 0 when everything matches; 1 with a list of mismatches otherwise.
The functions are also imported by tests/e2e/test_native.py.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def set_root(root: Path):
    """Point every path at the tree rooted at `root` (--root)."""
    global ROOT, SRC_OS_H, SDK_DIR, SDK_HEADERS, MAIN_C, TRAMPOLINES_C
    ROOT = Path(root).resolve()
    SRC_OS_H = ROOT / "src" / "os" / "os.h"
    SDK_DIR = ROOT / "sdk" / "native"
    SDK_HEADERS = [SDK_DIR / "terminal.h", SDK_DIR / "os.h"]
    MAIN_C = ROOT / "src" / "main.c"
    TRAMPOLINES_C = ROOT / "simulator" / "unicorn_trampolines.c"


set_root(ROOT)

_STRUCT_RE = re.compile(r"typedef\s+struct(?:\s+\w+)?\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
POINTER_SIZE = 4  # ARM32


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def _norm(decl: str) -> str:
    decl = re.sub(r"\s+", " ", decl).strip()
    return re.sub(r"\s*([*(),\[\]])\s*", r"\1", decl)


def parse_structs(*headers: Path) -> dict[str, list[str]]:
    """{typedef name: [normalised member declarations]} for every
    `typedef struct {...} name;` in the headers."""
    text = "\n".join(_strip_comments(Path(h).read_text()) for h in headers)
    out = {}
    for m in _STRUCT_RE.finditer(text):
        body, name = m.groups()
        out[name] = [_norm(d) for d in body.split(";") if d.strip()]
    return out


def abi_structs(structs: dict) -> dict:
    return {k: v for k, v in structs.items()
            if k == "PicoCalcAPI" or k == "pczip_stat_t"
            or re.fullmatch(r"picocalc_\w+_t", k)}


def api_layout(headers=None) -> dict:
    """The PicoCalcAPI layout a header declares, for ARM32:

    {"tables": [(field, type, size)], "size": sizeof(PicoCalcAPI),
     "version_off": offsetof(PicoCalcAPI, version)}

    Every sub-table is a list of function pointers (checked), so its size is
    4 bytes per member.
    """
    structs = parse_structs(*(headers or (SRC_OS_H,)))
    tables, off, version_off = [], 0, None
    for decl in structs["PicoCalcAPI"]:
        m = re.fullmatch(r"const (\w+)\*(\w+)", decl)
        if m:
            typ, field = m.groups()
            members = structs[typ]
            bad = [d for d in members if "(*" not in d]
            if bad:
                raise ValueError(f"{typ} has non-function-pointer members {bad}")
            tables.append((field, typ, POINTER_SIZE * len(members)))
            off += POINTER_SIZE
        elif decl == "uint32_t version":
            version_off = off
            off += 4
        else:
            raise ValueError(f"unexpected PicoCalcAPI member: {decl!r}")
    if version_off is None:
        raise ValueError("PicoCalcAPI has no version field")
    return {"tables": tables, "size": off, "version_off": version_off}


def api_version() -> int:
    """g_api.version as src/main.c sets it."""
    m = re.search(r"g_api\.version\s*=\s*(\d+)\s*;", MAIN_C.read_text())
    if not m:
        raise ValueError(f"no `g_api.version = N;` in {MAIN_C}")
    return int(m.group(1))


def check_headers() -> list[str]:
    src = abi_structs(parse_structs(SRC_OS_H))
    sdk = abi_structs(parse_structs(*SDK_HEADERS))
    problems = []
    for name in sorted(set(src) | set(sdk)):
        if name not in sdk:
            problems.append(f"{name}: in src/os/os.h but not in sdk/native")
        elif name not in src:
            problems.append(f"{name}: in sdk/native but not in src/os/os.h")
        elif src[name] != sdk[name]:
            a, b = src[name], sdk[name]
            i = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y),
                     min(len(a), len(b)))
            problems.append(
                f"{name}: member {i} differs: src/os/os.h "
                f"{a[i] if i < len(a) else '<end>'!r} vs sdk/native "
                f"{b[i] if i < len(b) else '<end>'!r} "
                f"({len(a)} vs {len(b)} members)")
    return problems


def check_trampolines(layout: dict, version: int) -> list[str]:
    text = TRAMPOLINES_C.read_text()
    problems = []
    m = re.search(r"api_struct_size\s*=\s*(\d+)\s*\*\s*4\s*;", text)
    if not m or int(m.group(1)) * 4 != layout["size"]:
        problems.append(
            f"unicorn_trampolines.c: api_struct_size "
            f"{m.group(0) if m else '<missing>'} != sizeof(PicoCalcAPI) "
            f"{layout['size']}")
    writes = {int(o): v for o, v in
              re.findall(r"write32\(uc,\s*api_base\s*\+\s*(\d+),\s*(\w+)\)", text)}
    got = writes.get(layout["version_off"])
    if got is None or not got.isdigit() or int(got) != version:
        problems.append(
            f"unicorn_trampolines.c: writes {got!r} at api_base + "
            f"{layout['version_off']} (offsetof(PicoCalcAPI, version)); "
            f"src/main.c sets g_api.version = {version}")
    # Every table pointer, including those appended after `version`.
    table_offs = set(range(0, layout["size"], POINTER_SIZE)) - {layout["version_off"]}
    missing = sorted(table_offs - set(writes))
    if missing:
        problems.append(f"unicorn_trampolines.c: no write32 at api_base + {missing}")
    return problems


def compiled_sizes(cc: str, headers_dir: Path, header: str, names) -> dict:
    """sizeof() of each struct in `names` plus offsetof(PicoCalcAPI, version),
    compiled for the target by `cc` and read back from symbol sizes."""
    nm = re.sub(r"gcc$", "nm", cc) if cc.endswith("gcc") else "nm"
    lines = ["#include <stddef.h>", f'#include "{header}"']
    for n in names:
        lines.append(f"char abi_sz_{n}[sizeof({n})];")
    lines.append("char abi_off_version[offsetof(PicoCalcAPI, version) + 1];")
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp) / "abi.c"
        o = Path(tmp) / "abi.o"
        c.write_text("\n".join(lines) + "\n")
        subprocess.run([cc, "-mcpu=cortex-m33", "-mthumb", "-fno-common",
                        "-I", str(headers_dir), "-c", str(c), "-o", str(o)],
                       check=True)
        out = subprocess.run([nm, "-S", str(o)], check=True,
                             capture_output=True, text=True).stdout
    sizes = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3].startswith("abi_"):
            sizes[parts[3]] = int(parts[1], 16)
    result = {n: sizes[f"abi_sz_{n}"] for n in names}
    result["offsetof(PicoCalcAPI, version)"] = sizes["abi_off_version"] - 1
    return result


def check_compiled(cc: str, layout: dict) -> list[str]:
    names = sorted(abi_structs(parse_structs(SRC_OS_H)))
    src = compiled_sizes(cc, SRC_OS_H.parent, "os.h", names)
    sdk = compiled_sizes(cc, SDK_DIR, "os.h", names)
    problems = [f"compiled {k}: src/os/os.h {src[k]} vs sdk/native {sdk[k]}"
                for k in src if src[k] != sdk[k]]
    if src["PicoCalcAPI"] != layout["size"]:
        problems.append(f"compiled sizeof(PicoCalcAPI) {src['PicoCalcAPI']} "
                        f"!= parsed {layout['size']}")
    if src["offsetof(PicoCalcAPI, version)"] != layout["version_off"]:
        problems.append("compiled offsetof(PicoCalcAPI, version) "
                        f"{src['offsetof(PicoCalcAPI, version)']} != parsed "
                        f"{layout['version_off']}")
    for _field, typ, size in layout["tables"]:
        if src[typ] != size:
            problems.append(f"compiled sizeof({typ}) {src[typ]} != parsed {size}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cc", help="ARM cross compiler for the compiled-layout "
                    "check (e.g. arm-none-eabi-gcc); skipped when omitted")
    ap.add_argument("--root", type=Path, default=ROOT,
                    help="repository root to check (default: this checkout)")
    args = ap.parse_args()
    set_root(args.root)

    layout = api_layout()
    version = api_version()
    problems = check_headers() + check_trampolines(layout, version)
    if args.cc:
        if not shutil.which(args.cc):
            problems.append(f"compiler not found: {args.cc}")
        else:
            problems += check_compiled(args.cc, layout)

    print(f"PicoCalcAPI: {len(layout['tables'])} tables, sizeof "
          f"{layout['size']}, version at +{layout['version_off']}, "
          f"g_api.version = {version}")
    for field, typ, size in layout["tables"]:
        print(f"  {field:12s} {typ:24s} {size:4d} bytes")
    if problems:
        print(f"\nABI drift ({len(problems)}):")
        for p in problems:
            print("  " + p)
        return 1
    print("ABI copies agree" + (" (compiled layout checked)" if args.cc else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
