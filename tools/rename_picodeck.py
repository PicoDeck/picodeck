#!/usr/bin/env python3
"""Rename PicOS to PicoDeck across a git checkout.

Design: specs/2026-09-26-picodeck-rename-design.md (section 4.3).  Rewrites
every tracked text file and `git mv`s every tracked path whose name carries the
old name.  Safe to re-run: no rule's output matches any rule.  Run from the
root of the checkout (or pass --root):

    python3 tools/rename_picodeck.py                          # rewrite in place
    python3 tools/rename_picodeck.py --check                  # exit 1 if an old name is left
    python3 tools/rename_picodeck.py --exclude docs/critic/   # also skip a prefix (repeatable)

Never touched: submodules (git tracks them as commit pointers), symlinks,
binaries (a NUL byte in the first 8 KiB), tests/fuzz/corpus/, and this script
and its test, which must keep the names they map from.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

DEFAULT_EXCLUDES = (
    "tests/fuzz/corpus/",
    "tools/rename_picodeck.py",
    "tests/unit/test_rename_picodeck.py",
)

APP_REPOS = "snake|blockexe|doom|cdogs|store"
# "picosdk" is the Pico SDK (Mongoose's MG_ARCH_PICOSDK, mg_picosdk_write), not us.
NOT_SDK = r"(?![dD][kK])"


def _wiki_url(m: re.Match) -> str:
    page = m.group(1)
    if not page or page.lower() == "home":
        return "https://picodeck.net/docs/"
    return f"https://picodeck.net/docs/{page.lower()}/"


# Applied in order: specific rules first, the general brand rules last.
RULES = [
    (re.compile(r"com\.picos\."), "net.picodeck."),
    (re.compile(r"picos\.jeffory\.dev"), "store.picodeck.net"),
    (re.compile(r"(?:https?://)?github\.com/jeffory/PicOS/wiki(?:/([A-Za-z0-9_-]+))?/?"), _wiki_url),
    (re.compile(r"jeffory/PicOS-Rally(?![-\w])"), "PicoDeck/rally"),
    (re.compile(r"jeffory/(?:PicOS|picOS)(?![-\w])"), "PicoDeck/picodeck"),
    (re.compile(r"jeffory/picos-apps(?![-\w])"), "PicoDeck/picodeck"),
    (re.compile(rf"jeffory/picos-({APP_REPOS})(?![-\w])"), r"PicoDeck/\1"),
    (re.compile(r"jeffory/(RP2350-GBC|doomgeneric)(?![-\w])"), r"PicoDeck/\1"),
    (re.compile(r"picocalc_os"), "picodeck"),
    (re.compile(r"PICOS_STORE_(BUCKET|KV)"), r"STORE_\1"),
    # Local checkouts: ~/Projects/<old> -> ~/Projects/PicoDeck/<new> (spec section 8).
    (re.compile(r"Projects/PicOS-Store(?![-\w])"), "Projects/PicoDeck/store"),
    (re.compile(r"Projects/PicOS-Rally(?![-\w])"), "Projects/PicoDeck/rally"),
    (re.compile(rf"Projects/picos-({APP_REPOS})(?![-\w])"), r"Projects/PicoDeck/\1"),
    (re.compile(r"Projects/PicOS(?![-\w])"), "Projects/PicoDeck/picodeck"),
    # The general brand rules, case-preserving.
    (re.compile("PicOS" + NOT_SDK), "PicoDeck"),
    (re.compile("picOS" + NOT_SDK), "picodeck"),
    (re.compile("PICOS" + NOT_SDK), "PICODECK"),
    (re.compile("Picos" + NOT_SDK), "Picodeck"),
    (re.compile("picos" + NOT_SDK), "picodeck"),
]

LEFTOVER = re.compile("[pP][iI][cC][oO][sS]" + NOT_SDK + r"|picocalc_os|jeffory\.dev")
# Binaries are never rewritten, but an app id or host compiled into one (a
# native app's /data path) breaks at run time: --check says rebuild it.
BINARY_LEFTOVER = re.compile(rb"com\.picos\.|picos\.jeffory\.dev")


def rename_text(text: str) -> str:
    """Apply every rule, in order."""
    for pattern, repl in RULES:
        text = pattern.sub(repl, text)
    return text


def leftovers(text: str) -> list[tuple[int, str]]:
    """(1-based line number, line) for each line still carrying an old name."""
    return [(n, line) for n, line in enumerate(text.splitlines(), 1)
            if LEFTOVER.search(line)]


def tracked_files(root: Path) -> list[str]:
    """Regular files in the index; submodules (160000) and symlinks (120000) are skipped."""
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-s", "-z"],
                         check=True, capture_output=True).stdout
    files = []
    for entry in filter(None, out.split(b"\0")):
        meta, path = entry.split(b"\t", 1)
        if meta.split()[0] in (b"100644", b"100755"):
            files.append(path.decode())
    return list(dict.fromkeys(files))


def excluded(path: str, excludes: tuple[str, ...]) -> bool:
    return any(path == e or path.startswith(e) for e in excludes)


def read_text(path: Path) -> tuple[str, str] | None:
    """(text, encoding), or None for a binary file.  Non-UTF-8 text is read as
    Latin-1, which round-trips every byte; the rules are ASCII."""
    data = path.read_bytes()
    if b"\0" in data[:8192]:
        return None
    try:
        return data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        return data.decode("latin-1"), "latin-1"


def apply(root: Path, excludes: tuple[str, ...]) -> tuple[int, int]:
    rewritten = moved = 0
    for rel in tracked_files(root):
        path = root / rel
        if excluded(rel, excludes) or not path.is_file():
            continue
        got = read_text(path)
        if got is not None:
            text, encoding = got
            new = rename_text(text)
            if new != text:
                path.write_bytes(new.encode(encoding))
                rewritten += 1
        new_rel = rename_text(rel)
        if new_rel != rel:
            if (root / new_rel).exists():
                sys.exit(f"refusing to rename {rel}: {new_rel} already exists")
            (root / new_rel).parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(["git", "-C", str(root), "mv", rel, new_rel], check=True)
            moved += 1
    return rewritten, moved


def check(root: Path, excludes: tuple[str, ...]) -> int:
    found = 0
    for rel in tracked_files(root):
        path = root / rel
        if excluded(rel, excludes) or not path.is_file():
            continue
        if LEFTOVER.search(rel):
            print(f"{rel}: old name in the path")
            found += 1
        got = read_text(path)
        if got is None:
            if BINARY_LEFTOVER.search(path.read_bytes()):
                print(f"{rel}: binary contains an old id or host (rebuild it)")
                found += 1
            continue
        for n, line in leftovers(got[0]):
            print(f"{rel}:{n}: {line.strip()[:160]}")
            found += 1
    if found:
        print(f"{found} old name(s) left: run tools/rename_picodeck.py", file=sys.stderr)
    return 1 if found else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Rename PicOS to PicoDeck across a git checkout.")
    ap.add_argument("--root", type=Path, default=Path.cwd(), help="checkout root (default: cwd)")
    ap.add_argument("--check", action="store_true", help="report old names left; exit 1 if any")
    ap.add_argument("--exclude", action="append", default=[], metavar="PREFIX",
                    help="also skip paths starting with PREFIX (repeatable)")
    args = ap.parse_args(argv)
    root = args.root.resolve()
    excludes = DEFAULT_EXCLUDES + tuple(args.exclude)
    if args.check:
        return check(root, excludes)
    rewritten, moved = apply(root, excludes)
    print(f"rewrote {rewritten} file(s), moved {moved} path(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
