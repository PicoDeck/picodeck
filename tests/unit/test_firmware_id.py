"""cmake/picodeck_firmware_id.cmake, the generator behind sys->getFirmwareId: run as a script
on small git repositories. It runs on every build, so what matters most is what it leaves
alone: a header that has not changed keeps its mtime, or every build would relink.
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "cmake" / "picodeck_firmware_id.cmake"

pytestmark = pytest.mark.skipif(not (shutil.which("cmake") and shutil.which("git")),
                                reason="needs cmake and git")


def git(repo: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t",
         "-c", "protocol.file.allow=always", *args],
        capture_output=True, text=True, check=True).stdout.strip()


def make_repo(path: Path) -> Path:
    path.mkdir()
    git(path, "init", "-q")
    (path / "tracked.c").write_text("int x;\n")
    git(path, "add", ".")
    git(path, "commit", "-qm", "first")
    return path


def generate(repo: Path, out: Path) -> str:
    subprocess.run(["cmake", f"-DROOT={repo}", f"-DOUT={out}", "-P", str(SCRIPT)], check=True)
    text = out.read_text()
    return text.split('PICODECK_FIRMWARE_ID "')[1].split('"')[0]


@pytest.fixture
def repo(tmp_path):
    return make_repo(tmp_path / "repo")


def test_a_clean_tree_is_the_short_hash(repo, tmp_path):
    assert generate(repo, tmp_path / "id.h") == git(repo, "rev-parse", "--short", "HEAD")


def test_a_changed_tracked_file_is_dirty(repo, tmp_path):
    (repo / "tracked.c").write_text("int y;\n")
    assert generate(repo, tmp_path / "id.h") == git(repo, "rev-parse", "--short", "HEAD") + "-dirty"


def test_a_staged_change_is_dirty(repo, tmp_path):
    (repo / "tracked.c").write_text("int y;\n")
    git(repo, "add", ".")
    assert generate(repo, tmp_path / "id.h").endswith("-dirty")


def test_an_untracked_file_is_not_dirty(repo, tmp_path):
    (repo / "scratch.txt").write_text("x\n")
    assert not generate(repo, tmp_path / "id.h").endswith("-dirty")


def test_a_touched_file_with_the_same_content_is_not_dirty(repo, tmp_path):
    os.utime(repo / "tracked.c", (1, 1))
    assert not generate(repo, tmp_path / "id.h").endswith("-dirty")


@pytest.fixture
def with_submodule(tmp_path):
    lib = make_repo(tmp_path / "lib")
    top = make_repo(tmp_path / "top")
    git(top, "submodule", "add", "-q", str(lib), "third_party/lib")
    git(top, "commit", "-qm", "add lib")
    return top


def test_untracked_content_in_a_submodule_is_not_dirty(with_submodule, tmp_path):
    """third_party/miniz: a build leaves a generated miniz_export.h in it."""
    sub = with_submodule / "third_party" / "lib"
    (sub / "generated.h").write_text("x\n")
    assert "third_party/lib" in git(with_submodule, "status", "--porcelain")  # git says so
    assert not generate(with_submodule, tmp_path / "id.h").endswith("-dirty")


def test_a_change_in_a_submodule_is_dirty(with_submodule, tmp_path):
    (with_submodule / "third_party" / "lib" / "tracked.c").write_text("int y;\n")
    assert generate(with_submodule, tmp_path / "id.h").endswith("-dirty")


def test_another_commit_in_a_submodule_is_dirty(with_submodule, tmp_path):
    sub = with_submodule / "third_party" / "lib"
    (sub / "tracked.c").write_text("int y;\n")
    git(sub, "add", ".")
    git(sub, "commit", "-qm", "second")
    assert generate(with_submodule, tmp_path / "id.h").endswith("-dirty")


def test_an_unchanged_id_leaves_the_header_alone(repo, tmp_path):
    out = tmp_path / "id.h"
    first = generate(repo, out)
    os.utime(out, (1000, 1000))
    assert generate(repo, out) == first
    assert out.stat().st_mtime == 1000


def test_a_new_id_rewrites_the_header(repo, tmp_path):
    out = tmp_path / "id.h"
    generate(repo, out)
    os.utime(out, (1000, 1000))
    (repo / "tracked.c").write_text("int y;\n")
    assert generate(repo, out).endswith("-dirty")
    assert out.stat().st_mtime != 1000


def test_no_git_is_unknown(tmp_path):
    (tmp_path / "plain").mkdir()
    env_ceiling = {**os.environ, "GIT_CEILING_DIRECTORIES": str(tmp_path)}
    out = tmp_path / "id.h"
    subprocess.run(["cmake", f"-DROOT={tmp_path / 'plain'}", f"-DOUT={out}", "-P", str(SCRIPT)],
                   check=True, env=env_ceiling)
    assert '"unknown"' in out.read_text()
