"""Unit tests for tests/e2e/hw_target.py's _manifest_key.

Run: python3 -m pytest tests/unit/test_hw_target_manifest_key.py -v

_manifest_key decides whether HwTarget.push_app must reboot the device: the
launcher caches app.json at boot, so a pushed manifest that differs (by this
key) from the one already on the card means the cache is stale. It must
therefore cover every app.json field the launcher actually uses to launch the
app (src/os/app_manifest.c, src/os/launcher.c), not just cosmetic ones.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "e2e"))
from hw_target import _manifest_key  # noqa: E402


def _manifest(**overrides):
    base = {
        "id": "com.example.app",
        "name": "Example",
        "requirements": ["http"],
        "min_psram_kb": 0,
        "system_clock_khz": 250000,
    }
    base.update(overrides)
    return base


def test_none_manifest_key_is_none():
    assert _manifest_key(None) is None


def test_identical_manifests_produce_the_same_key():
    a = _manifest()
    b = _manifest()
    assert _manifest_key(a) == _manifest_key(b)


def test_clock_change_produces_a_different_key():
    old = _manifest(system_clock_khz=250000)
    new = _manifest(system_clock_khz=300000)
    assert _manifest_key(old) != _manifest_key(new)


def test_requirements_order_does_not_matter():
    a = _manifest(requirements=["http", "audio"])
    b = _manifest(requirements=["audio", "http"])
    assert _manifest_key(a) == _manifest_key(b)


def test_cosmetic_fields_do_not_affect_the_key():
    a = _manifest()
    a["description"] = "old description"
    a["version"] = "1.0"
    a["author"] = "alice"
    a["category"] = "tools"
    b = _manifest()
    b["description"] = "brand new description"
    b["version"] = "2.0"
    b["author"] = "bob"
    b["category"] = "games"
    assert _manifest_key(a) == _manifest_key(b)
