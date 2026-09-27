"""The launcher draws app names and descriptions in the built-in 6x8 font
(src/fonts/font_6x8.c), which covers printable ASCII only: anything else, an
em dash or curly quote, comes out as a box. Run: python3 -m pytest tests/unit/test_app_text.py -v"""
import json
from pathlib import Path

import pytest

APPS = sorted((Path(__file__).resolve().parents[2] / "apps").glob("*/app.json"))


@pytest.mark.parametrize("manifest", APPS, ids=lambda p: p.parent.name)
def test_name_and_description_are_printable_ascii(manifest):
    app = json.loads(manifest.read_text(encoding="utf-8"))
    for field in ("name", "description"):
        text = app.get(field, "")
        missing = sorted({c for c in text if not " " <= c <= "~"})
        assert not missing, f"{field} {text!r} uses {missing}, which the 6x8 font can't draw"


def test_found_the_apps():
    assert len(APPS) > 10
