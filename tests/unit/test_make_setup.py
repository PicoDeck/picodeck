"""What a fresh clone needs from the Makefile. Run: python3 -m pytest tests/unit/test_make_setup.py

A plain `git clone` brings no submodules and no FatFS sources (they are
gitignored; only ffconf.h and port/diskio_spi.c are tracked), so `make setup`
has to fetch FatFS and the build targets have to name the missing submodules
instead of failing later inside CMake. These run the real Makefile in a small
scratch tree, with a stand-in for scripts/ci/fetch-fatfs.sh.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SUBMODULES = ["AnimatedGIF", "JPEGDEC", "PNGdec", "miniz", "tgx", "umm_malloc"]
FATFS_FETCHED = ["ff.c", "ff.h", "diskio.h", "ffsystem.c", "ffunicode.c"]


def make(tree, *args):
    return subprocess.run(["make", "-C", str(tree), *args],
                          capture_output=True, text=True, timeout=30)


@pytest.fixture
def tree(tmp_path):
    shutil.copy(ROOT / "Makefile", tmp_path / "Makefile")
    for name in SUBMODULES:
        (tmp_path / "third_party" / name).mkdir(parents=True)
        (tmp_path / "third_party" / name / "present").write_text("x")
    (tmp_path / "third_party" / "fatfs" / "port").mkdir(parents=True)
    (tmp_path / "third_party" / "fatfs" / "ffconf.h").write_text("tracked")
    script = tmp_path / "scripts" / "ci" / "fetch-fatfs.sh"
    script.parent.mkdir(parents=True)
    script.write_text("#!/bin/sh\necho ran >> fetched.log\n"
                      "cd third_party/fatfs && touch " + " ".join(FATFS_FETCHED) + "\n")
    script.chmod(0o755)
    return tmp_path


def fatfs_ran(tree):
    return (tree / "fetched.log").exists()


def test_download_fatfs_fetches_a_fresh_clone(tree):
    result = make(tree, "download-fatfs")
    assert result.returncode == 0, result.stdout + result.stderr
    assert fatfs_ran(tree)
    assert (tree / "third_party" / "fatfs" / "ff.c").exists()


def test_download_fatfs_is_not_fooled_by_a_partial_copy(tree):
    (tree / "third_party" / "fatfs" / "ff.h").write_text("only the header")
    assert make(tree, "download-fatfs").returncode == 0
    assert fatfs_ran(tree)


def test_download_fatfs_leaves_a_complete_copy_alone(tree):
    for name in FATFS_FETCHED:
        (tree / "third_party" / "fatfs" / name).write_text("x")
    result = make(tree, "download-fatfs")
    assert result.returncode == 0
    assert not fatfs_ran(tree)
    assert "already present" in result.stdout


def test_download_fatfs_fails_when_the_fetch_does(tree):
    script = tree / "scripts" / "ci" / "fetch-fatfs.sh"
    script.write_text("#!/bin/sh\nexit 7\n")
    result = make(tree, "download-fatfs")
    assert result.returncode != 0
    assert "Setup complete" not in result.stdout


@pytest.mark.parametrize("target", ["check-submodules", "simulator-check", "check-env"])
def test_an_empty_submodule_names_the_fix(tree, target):
    (tree / "third_party" / "PNGdec" / "present").unlink()
    result = make(tree, target)
    assert result.returncode != 0
    text = result.stdout + result.stderr
    assert "third_party/PNGdec" in text
    assert "git submodule update --init --recursive" in text
    assert "--recurse-submodules" in text


def test_submodules_present_pass(tree):
    result = make(tree, "check-submodules")
    assert result.returncode == 0, result.stdout + result.stderr


def test_test_unit_checks_submodules_first(tree):
    (tree / "third_party" / "miniz" / "present").unlink()
    result = make(tree, "test-unit")
    assert result.returncode != 0
    assert "git submodule update" in result.stdout + result.stderr


def test_build_fetches_its_sources_itself():
    # `make build` straight after a clone: Lua and FatFS, not a "no rule to
    # make target third_party/lua-5.4".
    out = subprocess.run(["make", "-C", str(ROOT), "-n", "build", "PICO_SDK_PATH=/nonexistent"],
                         capture_output=True, text=True, timeout=30).stdout
    assert "fetch-fatfs.sh" in out and "picodeck_lua.cmake" in out


def test_simulator_run_uses_the_checkout_as_the_card():
    # --sd-card is the card root, so "--sd-card ./apps" gave an empty launcher
    # and created apps/, data/ and system/ inside apps/.
    out = subprocess.run(["make", "-C", str(ROOT), "-n", "simulator-run"],
                         capture_output=True, text=True, timeout=30).stdout
    assert "--sd-card ./apps" not in out
    assert "build_sim/picodeck_simulator" in [line.strip() for line in out.splitlines()]
