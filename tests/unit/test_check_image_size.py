"""tools/check_image_size.py fails the firmware build when picodeck.bin is larger
than System Update accepts (OTA_MAX_SIZE in src/os/ota_verify.h), instead of the
device refusing the update later. Run: python3 -m pytest tests/unit/test_check_image_size.py -v
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "check_image_size.py"
HEADER = ROOT / "src" / "os" / "ota_verify.h"


def run(bin_path: Path, header: Path = HEADER):
    return subprocess.run([sys.executable, str(SCRIPT), str(bin_path), "--header", str(header)],
                          capture_output=True, text=True)


def test_under_cap_passes(tmp_path):
    b = tmp_path / "picodeck.bin"
    b.write_bytes(b"\0" * (1536 * 1024))
    res = run(b)
    assert res.returncode == 0, res.stderr
    assert res.stdout.strip() == "image 1572864 of 2097152 bytes (75%)"


def test_exactly_the_cap_passes(tmp_path):
    b = tmp_path / "picodeck.bin"
    b.write_bytes(b"\0" * (2 * 1024 * 1024))
    assert run(b).returncode == 0


def test_over_cap_fails_with_message(tmp_path):
    b = tmp_path / "picodeck.bin"
    b.write_bytes(b"\0" * (2 * 1024 * 1024 + 1))
    res = run(b)
    assert res.returncode == 1
    assert ("image 2097153 bytes exceeds OTA_MAX_SIZE 2097152: System Update would refuse it"
            in res.stderr)


def test_reads_the_real_header():
    sys.path.insert(0, str(ROOT / "tools"))
    import check_image_size as cis  # noqa: E402
    assert cis.read_cap(HEADER) == 2 * 1024 * 1024


def test_header_without_the_define_fails(tmp_path):
    b = tmp_path / "picodeck.bin"
    b.write_bytes(b"\0")
    h = tmp_path / "x.h"
    h.write_text("#define OTHER 1\n")
    res = run(b, h)
    assert res.returncode != 0
    assert "OTA_MAX_SIZE" in res.stderr
