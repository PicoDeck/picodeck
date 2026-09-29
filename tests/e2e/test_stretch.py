"""image:drawStretched — whole-image and sub-rect nearest-neighbour stretch."""
RED, GREEN, BLUE, WHITE, BLACK = 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000


def _px(sim, x, y):
    return sim.call("get_pixel", {"x": x, "y": y})["rgb565"]


def test_draw_stretched(simulator):
    simulator.clear_log()
    simulator.launch_app("stretch_test")
    simulator.wait_for_log("ST:READY", timeout=30)
    assert simulator.wait_for_log(r"ST:ERRORS \S+", timeout=5).endswith("ok")
    simulator.wait_frames(2)
    # 2x2 stretched to 100x60: quadrants
    assert _px(simulator, 10, 10) == RED
    assert _px(simulator, 90, 10) == GREEN
    assert _px(simulator, 10, 50) == BLUE
    assert _px(simulator, 90, 50) == WHITE
    assert _px(simulator, 49, 29) == RED and _px(simulator, 50, 30) == WHITE
    # sub-rect (1,1,1,1) = white filling 30x30
    assert _px(simulator, 120, 0) == WHITE and _px(simulator, 149, 29) == WHITE
    assert _px(simulator, 150, 10) == BLACK
    # empty destination and off-image source draw nothing
    assert _px(simulator, 165, 10) == BLACK
    assert _px(simulator, 210, 10) == BLACK
    simulator.keypress("esc")
    simulator.wait_for_exit(timeout=10)
