"""E2E tests for picocalc.graphics (tilemap drawing, blinker arguments).

Drives the graphics_test fixture and asserts on its "GT" log markers plus
get_pixel probes of the drawn tilemap.
"""
import pytest

# RGB888 as the sim reports it for the RGB565 tile colours.
RED, GREEN, BLUE, YELLOW = (248, 0, 0), (0, 252, 0), (0, 0, 248), (248, 252, 0)


def _assert_near(px, expected, label, tolerance=32):
    got = (px["r"], px["g"], px["b"])
    assert all(abs(a - b) <= tolerance for a, b in zip(got, expected)), (
        f"{label}: expected ~{expected}, got {got}")


def _lines(sim):
    return [l if isinstance(l, str) else l.get("text", "")
            for l in sim.get_log_buffer()["lines"]]


@pytest.fixture
def graphics_app(simulator):
    simulator.clear_log()
    simulator.launch_app("graphics_test")
    simulator.wait_for_log("GT DONE", timeout=20)
    yield simulator
    simulator.keypress("esc")


def test_tilemap_draws_every_tile_from_its_source_rect(graphics_app):
    """A 2x2 tilemap in reversed tile order must show all four tileset
    tiles, each at its map cell (tiles 2-4 have non-zero source offsets)."""
    joined = "\n".join(_lines(graphics_app))
    assert "GT TILEMAP_READY" in joined, joined
    # Map cell (col,row) -> tile index: (0,0)=4, (1,0)=3, (0,1)=2, (1,1)=1.
    probes = [((16, 16), YELLOW, "cell 0,0 = tile 4"),
              ((48, 16), BLUE,   "cell 1,0 = tile 3"),
              ((16, 48), GREEN,  "cell 0,1 = tile 2"),
              ((48, 48), RED,    "cell 1,1 = tile 1")]
    for (x, y), colour, label in probes:
        _assert_near(graphics_app.call("get_pixel", {"x": x, "y": y}),
                     colour, label)


def test_blinker_start_reads_durations_after_self(graphics_app):
    """blinker:start(on, off) takes on/off from arguments 2 and 3; 200ms
    into a 50ms-on/5000ms-off cycle the blinker must be off."""
    joined = "\n".join(_lines(graphics_app))
    assert "GT BLINKER_ERR" not in joined, joined
    assert "GT BLINKER_OK state=false" in joined, joined


def _marker(sim, name):
    line = next((l for l in _lines(sim) if f"GT {name}_" in l), None)
    assert line is not None, "\n".join(_lines(sim))
    return line.split(f"GT {name}_", 1)[1]


def test_animator_four_argument_form_with_named_easing(graphics_app):
    """animator.new(duration, from, to, easing): the docs' own example. The
    new userdata must not be read as an argument."""
    assert _marker(graphics_app, "ANIM4") == "OK 100.0 true"


def test_animator_nil_easing_is_absent(graphics_app):
    assert _marker(graphics_app, "ANIMNIL") == "OK 100.0 true"


def test_animator_delay_holds_the_start_value_then_animates(graphics_app):
    """A 400 ms delay: the value stays at `from` with progress 0 (no uint32
    wrap ending it at once), then reaches `to` after delay + duration."""
    assert _marker(graphics_app, "ANIMDELAY") == "OK 10.0/0.0 90.0/true"
