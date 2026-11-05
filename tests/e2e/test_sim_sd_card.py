"""--sd-card names the card's root, with apps in <root>/apps/<name>/.

Before this the simulator took any path without a word: the apps folder itself
gave an empty launcher and created apps/, data/ and system/ inside it, a
missing folder gave an empty launcher, and a published binary with no flag
pointed at the CI runner's checkout it was built in. These cases stop before
any window opens, naming the fix, and touch nothing.
"""

import os
import re
import shutil
import subprocess
import time

import pytest

from picodeck_simulator import PicodeckSimulator

BINARY = str(PicodeckSimulator.DEFAULT_BINARY)


def run(args, binary=BINARY, cwd=None):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    return subprocess.run([binary, "--port", "0", "--unix-socket", "none", *args],
                          capture_output=True, text=True, timeout=20, env=env, cwd=cwd)


def app_dir(parent, name="myapp"):
    d = parent / name
    d.mkdir(parents=True)
    (d / "app.json").write_text('{"name": "My App"}')
    (d / "main.lua").write_text("return\n")
    return d


def test_the_apps_folder_is_refused_and_left_alone(tmp_path):
    apps = tmp_path / "apps"
    app_dir(apps)
    result = run(["--sd-card", str(apps)])
    assert result.returncode == 2, result.stdout + result.stderr
    assert "card's root" in result.stderr
    assert "apps/<name>" in result.stderr
    assert str(tmp_path) in result.stderr
    assert sorted(p.name for p in apps.iterdir()) == ["myapp"]


@pytest.mark.parametrize("entry", ["main.lua", "main.elf", "app.json"])
def test_any_app_file_marks_an_apps_folder(tmp_path, entry):
    folder = tmp_path / "mine"
    (folder / "game").mkdir(parents=True)
    (folder / "game" / entry).write_text("x")
    assert run(["--sd-card", str(folder)]).returncode == 2


def test_a_missing_folder_is_refused(tmp_path):
    missing = tmp_path / "nope"
    result = run(["--sd-card", str(missing)])
    assert result.returncode == 2
    assert str(missing) in result.stderr
    assert not missing.exists()


def test_a_file_is_refused(tmp_path):
    f = tmp_path / "card.img"
    f.write_text("x")
    result = run(["--sd-card", str(f)])
    assert result.returncode == 2
    assert str(f) in result.stderr


def wait_for_scan(proc, log, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        match = re.search(r"\[LAUNCHER\] Found (\d+) apps", log.read_text(errors="replace"))
        if match:
            return int(match.group(1))
        assert proc.poll() is None, log.read_text(errors="replace")
        time.sleep(0.05)
    raise AssertionError("no launcher scan: " + log.read_text(errors="replace"))


def start(args, log):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
    with open(log, "wb") as out:
        return subprocess.Popen([BINARY, "--port", "0", "--unix-socket", "none", *args],
                                stdout=out, stderr=subprocess.STDOUT, env=env)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def test_an_empty_folder_is_a_blank_card(tmp_path):
    card = tmp_path / "card"
    card.mkdir()
    proc = start(["--sd-card", str(card)], tmp_path / "log")
    try:
        assert wait_for_scan(proc, tmp_path / "log") == 0
    finally:
        stop(proc)
    assert {p.name for p in card.iterdir()} >= {"apps", "data", "system"}


def test_a_card_root_lists_its_apps(tmp_path):
    card = tmp_path / "card"
    app_dir(card / "apps")
    proc = start(["--sd-card", str(card)], tmp_path / "log")
    try:
        assert wait_for_scan(proc, tmp_path / "log") == 1
    finally:
        stop(proc)


def build_path(binary=BINARY):
    """The compiled-in default card: the checkout the binary was built from."""
    text = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=10).stdout
    match = re.search(r"source tree it was built from:\s*(\S+)\)", text)
    assert match, text
    return match.group(1)


def test_no_flag_on_another_machine_stops_and_says_why(tmp_path):
    # A published binary: the same bytes with the build path swapped for one
    # that is not on this computer, as the CI runner's is not.
    root = build_path()
    gone = ("/nonexistent/" + "x" * len(root))[:len(root)]
    binary = tmp_path / "picodeck-simulator-linux"
    binary.write_bytes(open(BINARY, "rb").read().replace(root.encode(), gone.encode()))
    binary.chmod(0o755)

    result = run([], binary=str(binary), cwd=tmp_path)
    assert result.returncode == 2, result.stdout + result.stderr
    assert "--sd-card" in result.stderr
    assert "apps/<name>" in result.stderr
    # Nothing appeared in the current directory, and --help is not misleading.
    assert sorted(p.name for p in tmp_path.iterdir()) == [binary.name]
    help_text = subprocess.run([str(binary), "--help"], capture_output=True, text=True, timeout=10).stdout
    assert "required" in help_text


def test_no_flag_in_a_checkout_uses_the_checkout(tmp_path):
    # `make simulator-run`: the repository root is the card, apps/ included.
    if not os.path.isdir(os.path.join(build_path(), "apps")):
        pytest.skip("simulator not built from this tree")
    proc = start([], tmp_path / "log")
    try:
        assert wait_for_scan(proc, tmp_path / "log") > 0
    finally:
        stop(proc)
