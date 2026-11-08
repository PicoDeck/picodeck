"""tools/card_files.py packs the App Store, System Update and /system/lib into the
firmware (issue #92). Run: python3 -m pytest tests/unit/test_card_files_gen.py -v

The firmware writes these files to the card at boot (src/os/card_files.c), so what
the generator emits must be exactly the repo's files, byte for byte, under the card
paths the firmware owns, and its id must change whenever any of them does.
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import card_files as gen  # noqa: E402

SCRIPT = ROOT / "tools" / "card_files.py"

# card_files.h's types, as Task 2 of the plan defines them, so the generated C can be
# compiled here before the firmware build does.
STUB_HEADER = """#pragma once
#include <stdint.h>
typedef struct { const char *path; const uint8_t *data; uint32_t len; } card_file_t;
typedef struct {
    const char *id; const card_file_t *files; int count;
    const char *const *roots; int nroots;
} card_bundle_t;
"""

ARRAY_RE = re.compile(r"static const uint8_t (f\d+)\[\d*\] = \{([^}]*)\};")
ENTRY_RE = re.compile(r'\{"([^"]+)", (f\d+), (\d+)u\}')


@pytest.fixture
def repo_root():
    return ROOT


@pytest.fixture
def tmp_repo(tmp_path):
    """The four bundled sources, small."""
    r = tmp_path / "repo"
    files = {
        "apps/store/app.json": b'{"id":"net.picodeck.store"}',
        "apps/store/main.lua": b"return\n",
        "apps/store/icon.png": bytes(range(256)),
        "apps/updater/app.json": b'{"id":"net.picodeck.updater"}',
        "apps/updater/main.lua": b"",
        "system/lib/download.lua": b"return {}\n",
        "system/lib/fonts/f.pfn": b"\x00\x01\x02",
        "sdk/fonts/spleen/LICENSE": b"BSD 2-Clause\n",
    }
    for rel, data in files.items():
        p = r / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return r


def source_of(card: str, repo: Path = ROOT) -> Path:
    for rel, dst in gen.BUNDLE:  # a single file's entry first: it sits inside /system/lib
        if card == dst:
            return repo / rel
    for rel, dst in gen.BUNDLE:
        if card.startswith(dst + "/"):
            return repo / rel / card[len(dst) + 1:]
    raise AssertionError(f"{card} is not under any bundled path")


def run_cli(repo: Path, out: Path):
    return subprocess.run([sys.executable, str(SCRIPT), "--repo", str(repo), "--out", str(out)],
                          capture_output=True, text=True, check=True)


def parse_c(text: str) -> dict:
    """{card path: bytes} from the generated source."""
    arrays = {name: bytes(int(b, 16) for b in body.replace(",", " ").split())
              for name, body in ARRAY_RE.findall(text)}
    out = {}
    for path, name, n in ENTRY_RE.findall(text):
        out[path] = arrays[name][:int(n)]
        assert len(out[path]) == int(n), path
    return out


def test_bundle_is_the_four_sources():
    assert gen.BUNDLE == [
        ("apps/store", "/apps/store"),
        ("apps/updater", "/apps/updater"),
        ("system/lib", "/system/lib"),
        ("sdk/fonts/spleen/LICENSE", "/system/lib/fonts/spleen-LICENSE"),
    ]
    assert gen.ROOTS == ("/apps/store/", "/apps/updater/", "/system/lib/")


def test_real_bundle_is_exactly_the_repo_files(repo_root):
    files = gen.collect(repo_root)
    paths = [p for p, _ in files]
    assert paths == sorted(paths)
    assert "/apps/store/main.lua" in paths and "/apps/updater/main.lua" in paths
    assert "/system/lib/download.lua" in paths and "/system/lib/fonts/spleen-5x8.pfn" in paths
    assert "/system/lib/fonts/spleen-LICENSE" in paths
    for card, data in files:
        assert data == source_of(card).read_bytes()  # byte for byte
    assert all(any(p.startswith(r) for r in gen.ROOTS) for p in paths)
    # every file under the bundled directories, none left out
    on_disk = sorted(
        "/" + p.relative_to(repo_root).as_posix()
        for rel in ("apps/store", "apps/updater", "system/lib")
        for p in (repo_root / rel).rglob("*") if p.is_file() and not p.name.startswith("."))
    assert sorted(p for p in paths if not p.endswith("spleen-LICENSE")) == on_disk


def test_rendered_c_round_trips_every_byte(repo_root, tmp_path):
    out = tmp_path / "gen" / "card_files_data.c"
    res = run_cli(repo_root, out)
    files = gen.collect(repo_root)
    text = out.read_text()
    assert parse_c(text) == dict(files)
    bid = gen.bundle_id(files)
    assert f'"{bid}"' in text
    assert re.fullmatch(r"card_files: %d files, %d bytes, id %s\n"
                        % (len(files), sum(len(d) for _, d in files), bid[:8]), res.stdout)


def test_rendered_c_compiles_cleanly(tmp_repo, tmp_path):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    (tmp_path / "card_files.h").write_text(STUB_HEADER)
    out = tmp_path / "card_files_data.c"
    run_cli(tmp_repo, out)
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                    "-I", str(tmp_path), str(out)], check=True)


def test_empty_file_is_bundled_with_length_zero(tmp_repo, tmp_path):
    out = tmp_path / "c.c"
    run_cli(tmp_repo, out)
    assert parse_c(out.read_text())["/apps/updater/main.lua"] == b""


def test_id_is_stable_and_changes_with_one_byte(tmp_repo):
    a = gen.bundle_id(gen.collect(tmp_repo))
    assert a == gen.bundle_id(gen.collect(tmp_repo))
    assert re.fullmatch(r"[0-9a-f]{64}", a)
    p = tmp_repo / "apps/store/icon.png"
    data = bytearray(p.read_bytes())
    data[100] ^= 1
    p.write_bytes(bytes(data))
    assert gen.bundle_id(gen.collect(tmp_repo)) != a


def test_id_changes_when_a_file_moves(tmp_repo):
    a = gen.bundle_id(gen.collect(tmp_repo))
    (tmp_repo / "system/lib/download.lua").rename(tmp_repo / "system/lib/dl.lua")
    assert gen.bundle_id(gen.collect(tmp_repo)) != a


def test_unchanged_output_is_not_rewritten(tmp_repo, tmp_path):
    out = tmp_path / "c.c"
    run_cli(tmp_repo, out)
    before = out.stat().st_mtime_ns
    run_cli(tmp_repo, out)
    assert out.stat().st_mtime_ns == before
    (tmp_repo / "apps/store/main.lua").write_bytes(b"return 1\n")
    run_cli(tmp_repo, out)
    assert parse_c(out.read_text())["/apps/store/main.lua"] == b"return 1\n"


def test_dotfiles_are_skipped(tmp_repo):
    (tmp_repo / "apps/store/.DS_Store").write_bytes(b"junk")
    (tmp_repo / "system/lib/.hidden").mkdir()
    (tmp_repo / "system/lib/.hidden/x.lua").write_bytes(b"x")
    paths = [p for p, _ in gen.collect(tmp_repo)]
    assert not any("/." in p for p in paths)


def test_overlong_path_is_refused(tmp_repo):
    (tmp_repo / "system/lib" / ("x" * 150 + ".lua")).write_bytes(b"")
    with pytest.raises(SystemExit, match="card_files: .*150"):
        gen.collect(tmp_repo)


def test_non_ascii_path_is_refused(tmp_repo):
    (tmp_repo / "system/lib/café.lua").write_bytes(b"")
    with pytest.raises(SystemExit, match="card_files: .*ASCII"):
        gen.collect(tmp_repo)


def test_missing_source_is_refused(tmp_repo):
    shutil.rmtree(tmp_repo / "apps/updater")
    with pytest.raises(SystemExit, match="card_files: .*apps/updater"):
        gen.collect(tmp_repo)
