"""tools/make_splash_logo.py: the committed splash_logo_cells.{h,c} are what
the generator writes now, and their palette and planes are sane."""
import re
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("numpy")
pytest.importorskip("scipy")

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "make_splash_logo.py"
OS = ROOT / "src" / "os"


def test_committed_files_are_current(tmp_path):
    subprocess.run([sys.executable, str(TOOL), str(tmp_path)], check=True)
    for name in ("splash_logo_cells.h", "splash_logo_cells.c"):
        assert (tmp_path / name).read_text() == (OS / name).read_text(), name


def _array(src, name):
    body = re.search(name + r"\[[^\]]*\] = \{(.*?)\};", src, re.S).group(1)
    return [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", body)]


def test_palette_runs_black_to_white():
    pal = _array((OS / "splash_logo_cells.c").read_text(), "splash_palette")
    assert len(pal) == 64
    assert pal[0] == 0x0000 and pal[63] == 0xFFFF

    def luma(c):
        return ((c >> 11) & 31) * 2 + ((c >> 5) & 63) + (c & 31)
    assert all(luma(a) <= luma(b) for a, b in zip(pal, pal[1:]))


def test_planes():
    h = (OS / "splash_logo_cells.h").read_text()
    rows = int(re.search(r"#define SPLASH_LOGO_ROWS (\d+)", h).group(1))
    src = (OS / "splash_logo_cells.c").read_text()
    g, f, b = (_array(src, n) for n in ("splash_logo_glyph", "splash_logo_fg", "splash_logo_bg"))
    assert len(g) == len(f) == len(b) == rows * 80
    assert all(fg >= bg for fg, bg in zip(f, b))
    assert all(gl == 0 for gl, fg, bg in zip(g, f, b) if fg == bg)
    assert all(gl != 0 for gl, fg, bg in zip(g, f, b) if fg != bg)
    assert max(f) == round(0.86 * 255)  # the letters
