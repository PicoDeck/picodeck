"""E2E tests for the panels.lua interactive-comics framework.

Drives the text-only panels_test fixture comic through every state machine
transition — advance, choice branch, story-var payoff, save on exit, resume
on relaunch — asserting on the library's own "PANELS:" log markers plus the
fixture's "PT " markers. panels.lua is staged onto the test SD from
system/lib/ (the canonical copy) at test time, exactly as firmware ships it.
"""
import re
import shutil
import time
from pathlib import Path

import pytest

# The simulator runs these tests on a paused virtual clock (--virtual-time,
# multiplier 0): panels.lua's animations are timed with sys.getTimeMs() and
# its frame loop never sleeps, so the clock moves only when the test calls
# step_time. Every timed wait is an exact step plus a frame barrier, never a
# wall-clock sleep. Durations come from Panels.Settings in system/lib/panels.lua.
ADVANCE_MS = 400      # advance-mode snap animation (input dropped while it runs)
TRANSITION_MS = 450   # each half of a fadeToBlack sequence change
SCROLL_STEP_MS = 100  # panels caps a frame's dt at 100 ms (scrollSpeed 180 px/s)

REPO_ROOT = Path(__file__).resolve().parents[2]


def _texts(sim):
    return [e["text"] for e in sim.get_log_lines(0)]


def _wait_for(sim, marker, timeout=15.0):
    """Wait for a log line containing `marker`; returns the whole log."""
    sim.wait_for_log(re.escape(marker), timeout=timeout)
    return "\n".join(_texts(sim))


def _wait_for_count(sim, marker, count, timeout=20.0):
    """Wait until `marker` has appeared in `count` log lines."""
    deadline = time.monotonic() + timeout
    since = 0
    while True:
        joined = "\n".join(_texts(sim))
        if joined.count(marker) >= count:
            return joined
        if time.monotonic() >= deadline:
            pytest.fail(f"marker {marker!r} seen fewer than {count} times "
                        f"within {timeout}s:\n" + "\n".join(_texts(sim)[-30:]))
        # Block on the next log line rather than polling on a timer.
        since = sim.get_log_buffer(tail=1)["next_seq"]
        try:
            sim.wait_for_log(".*", timeout=max(0.1, deadline - time.monotonic()),
                             since_seq=since)
        except TimeoutError:
            pass


def _inject(sim, button, action):
    """press/release `button` and wait until the OS input layer has read it.
    Explicit press/release (not click) never depends on the sim clock, which
    the tests keep paused: a click's 80 ms auto-release would never come."""
    r = sim.call("inject_button", {"button": button, "action": action})
    sim.wait_input_consumed(r["input_seq"], timeout=5.0)


def _tap(sim, key):
    """One press edge: panels reads the press in one frame and the release
    in a later one, so the press has been handled when this returns."""
    _inject(sim, key, "press")
    _inject(sim, key, "release")


def _frame_barrier(sim):
    """Return once a whole panels frame has run after this call: the press
    of an unused key is read by frame N (which then updates and flushes),
    its release by frame N+1."""
    _tap(sim, "tab")


def _advance(sim, ms):
    """Move the paused clock by `ms`, then let a frame see the new time."""
    sim.step_time(ms)
    _frame_barrier(sim)


def _paused_sim(sim_factory, test_sd_card):
    return sim_factory(test_sd_card, virtual_time=True)


def _stage_panels_lib(test_sd_card):
    src = REPO_ROOT / "system" / "lib" / "panels.lua"
    assert src.exists(), "system/lib/panels.lua missing from the repo"
    dest = test_sd_card / "system" / "lib"
    dest.mkdir(parents=True, exist_ok=True)
    # These tests keep the clock paused, where a sleep waits for a step, and
    # drive frames with keys: the idle sleep is off here, and the one test that
    # needs it stages the library as it is.
    text = src.read_text()
    assert "idleSleepMs      = 16," in text
    (dest / "panels.lua").write_text(text.replace("idleSleepMs      = 16,", "idleSleepMs      = 0,"))


def test_panels_full_lifecycle(sim_factory, test_sd_card):
    _stage_panels_lib(test_sd_card)
    simulator = _paused_sim(sim_factory, test_sd_card)

    simulator.launch_app("panels_test")
    joined = _wait_for(simulator, "PANELS:SEQ 1")
    assert "PT NOLIB" not in joined
    assert "PT START" in joined
    simulator.set_time_multiplier(0)

    # Custom renderFunction ran with the panel clip active.
    _wait_for(simulator, "PT RENDERFN")
    # A misspelled animate.ease is logged, not silently linear (#56).
    _wait_for(simulator, 'PANELS:ERR unknown ease "cubik", using linear')
    _advance(simulator, TRANSITION_MS)   # any opening fade has finished

    # Advance mode: forward key steps panels, then crosses into sequence 2.
    _tap(simulator, "down")              # panel 1 -> panel 2 (snap starts)
    _advance(simulator, ADVANCE_MS)      # snap done; input accepted again
    _tap(simulator, "down")              # past last panel -> fade out
    simulator.step_time(TRANSITION_MS)   # SEQ 2 logs at the fade midpoint
    _wait_for(simulator, "PANELS:SEQ 2")
    _advance(simulator, TRANSITION_MS)   # fade-in done; the choice arms
    _frame_barrier(simulator)            # on the first frame after it

    # Choice UI: select the SECOND option, confirm, land in sequence 3 with
    # the story var set by that choice.
    _tap(simulator, "down")              # highlight "second"
    _tap(simulator, "enter")
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:CHOICE 2 -> seq 3" in joined
    simulator.step_time(TRANSITION_MS)
    _wait_for(simulator, "PT PAYOFF picked=b")

    # ESC saves progress and exits cleanly back to the launcher.
    _tap(simulator, "esc")
    joined = _wait_for(simulator, "PT EXIT quit")
    assert "PANELS:EXIT quit" in joined
    assert joined.count("PANELS:ERR unknown ease") == 1, joined
    assert simulator.wait_for_exit(timeout=10.0)["result"] == "returned"

    # Relaunch: resume=true restores the saved sequence, not sequence 1.
    simulator.clear_log()
    simulator.launch_app("panels_test")
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:SEQ 1" not in joined, "resume went back to the start"

    _tap(simulator, "esc")
    _wait_for(simulator, "PT EXIT quit")


def _pad_tap(sim, state):
    """One press edge from the test pad, read by a frame: hold, let a whole
    frame run, release, let another run. The paused clock never retires a
    timed hold, so the release is explicit."""
    sim.pad(state)
    _frame_barrier(sim)
    sim.pad("none")
    _frame_barrier(sim)


def test_panels_driven_by_a_controller(sim_factory, test_sd_card):
    """The test pad (a controller's path) steps panels, picks a choice with
    A and leaves with B, as the keyboard's Down, Enter and Esc do."""
    _stage_panels_lib(test_sd_card)
    simulator = _paused_sim(sim_factory, test_sd_card)

    simulator.launch_app("panels_test")
    _wait_for(simulator, "PANELS:SEQ 1")
    simulator.set_time_multiplier(0)
    _advance(simulator, TRANSITION_MS)

    _pad_tap(simulator, "down")          # panel 1 -> panel 2
    _advance(simulator, ADVANCE_MS)
    _pad_tap(simulator, "down")          # past the last panel -> fade out
    simulator.step_time(TRANSITION_MS)
    _wait_for(simulator, "PANELS:SEQ 2")
    _advance(simulator, TRANSITION_MS)
    _frame_barrier(simulator)

    _pad_tap(simulator, "down")          # highlight "second"
    _pad_tap(simulator, "a")             # A confirms, like Enter
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for(simulator, "PANELS:SEQ 3")
    assert "PANELS:CHOICE 2 -> seq 3" in joined
    simulator.step_time(TRANSITION_MS)
    _wait_for(simulator, "PT PAYOFF picked=b")

    _pad_tap(simulator, "b")             # B leaves, like Esc
    _wait_for(simulator, "PT EXIT quit")
    simulator.pad("off")
    assert simulator.wait_for_exit(timeout=10.0)["result"] == "returned"


def _scroll(sim, frames):
    """Hold the forward key for `frames` frames of SCROLL_STEP_MS each: the
    paused clock gives exactly one frame per step a non-zero dt, so each
    step scrolls exactly SCROLL_STEP_MS * 180 px/s = 18 px."""
    _inject(sim, "down", "press")
    for _ in range(frames):
        _advance(sim, SCROLL_STEP_MS)
    _inject(sim, "down", "release")
    _frame_barrier(sim)


def _wait_screen(sim, pred, what, timeout=5.0):
    """Poll screenshots until pred(png) holds (a condition, not a delay)."""
    deadline = time.monotonic() + timeout
    while True:
        shot = sim.screenshot()
        if pred(shot):
            return shot
        if time.monotonic() >= deadline:
            pytest.fail(what)
        time.sleep(0.02)


def test_panels_hw_scroll(sim_factory, test_sd_card):
    """Rigid vertical scroll sequences must engage the LCD hardware-scroll
    ring (PANELS:HW on), stay pixel-stable when idle, survive a system-menu
    takeover (foreign register write -> resync -> re-enter), and release the
    ring cleanly (PANELS:HW off) when the end-of-sequence transition fires."""
    _stage_panels_lib(test_sd_card)
    simulator = _paused_sim(sim_factory, test_sd_card)
    simulator.clear_log()
    simulator.launch_app("panels_hw_test")
    joined = _wait_for(simulator, "PANELS:HW on")
    assert "PHW NOLIB" not in joined
    simulator.set_time_multiplier(0)
    _frame_barrier(simulator)

    top = simulator.screenshot()

    # Scroll in: the composed screen must change.
    _scroll(simulator, 4)                # 72 px
    mid = simulator.screenshot()
    assert mid != top, "hardware scroll did not move the screen"

    # Idle ring: no input, no redraws — frames must be pixel-identical,
    # however much time passes.
    _advance(simulator, 1000)
    assert simulator.screenshot() == mid, "ring not static without input"

    # System menu takeover: the OS resets the scroll register.  panels must
    # detect the foreign write via the write counter, repaint, and re-enter
    # the fast path (second "PANELS:HW on"), restoring the exact frame.
    r = simulator.keypress("menu")
    simulator.wait_input_consumed(r["input_seq"], timeout=5.0)
    _wait_screen(simulator, lambda s: s != mid, "system menu did not appear")
    _tap(simulator, "esc")               # read by the menu: closes it
    _wait_for_count(simulator, "PANELS:HW on", 2)
    _frame_barrier(simulator)
    assert simulator.screenshot() == mid, (
        "frame not restored after system-menu takeover"
    )

    # Drive past maxScroll (446px): the clamped edge press starts the cut
    # transition (PANELS:HW off), which loops back into sequence 1 and
    # re-enters the fast path (third "PANELS:HW on").
    _scroll(simulator, 24)               # 72 + 432 px: clamped at 446
    _tap(simulator, "down")              # forward press at the end: advance
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for_count(simulator, "PANELS:SEQ 1", 2)
    assert "PANELS:HW off" in joined, (
        "transition started without releasing the hardware-scroll ring"
    )
    _wait_for_count(simulator, "PANELS:HW on", 3)

    _tap(simulator, "esc")
    _wait_for(simulator, "PHW EXIT quit")


RIGID_COMIC = r"""
local Panels = picocalc.sys.loadlib("panels")
local disp = picocalc.display
local comicData = {
    name = "EmbedDoc",
    sequences = {
        {
            scrollType = "scroll",
            nextSequence = 1,
            transition = "cut",
            panels = {
                { frame = { height = 250 }, backgroundColor = disp.rgb(200, 40, 40),
                  layers = { { text = "TOP", x = 140, y = 20 } } },
                { frame = { height = 250, marginBefore = 8 }, backgroundColor = disp.rgb(40, 180, 60),
                  layers = { { text = "MID", x = 140, y = 120 } } },
                { frame = { height = 250, marginBefore = 8 }, backgroundColor = disp.rgb(40, 80, 200),
                  layers = { { text = "BOTTOM", x = 130, y = 200 } } },
            },
        },
    },
}
local opts = { resume = false }
"""


def _scroll_shots(sim):
    """The screen at rest, and after scrolling 4 and 8 steps."""
    _wait_for(sim, "PANELS:HW on")
    sim.set_time_multiplier(0)
    _frame_barrier(sim)
    shots = [sim.screenshot()]
    for _ in range(2):
        _scroll(sim, 4)
        shots.append(sim.screenshot())
    return shots


def test_panels_doc_embed_loop_matches_start(sim_factory, test_sd_card, tmp_path):
    """The loop on the page, verbatim, on a hardware-scroll sequence: the same
    pixels as Panels.start, and Esc gets out of it. The unconditional flush the
    page used to show scrambled this screen."""
    from helpers import PROJECT_ROOT, stage_lua_app
    text = (PROJECT_ROOT / "docs" / "Library-Panels.md").read_text()
    section = text.split("### `Panels.new(comicData [, opts])`", 1)[1]
    loop = section.split("```lua\n", 1)[1].split("```", 1)[0]

    _stage_panels_lib(test_sd_card)
    stage_lua_app(test_sd_card, "embed_start",
                  RIGID_COMIC + 'Panels.start(comicData, opts)\n')
    stage_lua_app(test_sd_card, "embed_doc", RIGID_COMIC + loop)

    sim = _paused_sim(sim_factory, test_sd_card)
    sim.clear_log()
    sim.launch_app("embed_start")
    want = _scroll_shots(sim)
    assert want[1] != want[0] and want[2] != want[1]
    sim.set_time_multiplier(50)
    sim.keypress("esc")
    sim.wait_for_exit(timeout=10)

    sim.clear_log()
    sim.launch_app("embed_doc")
    got = _scroll_shots(sim)
    assert got == want
    sim.set_time_multiplier(50)
    sim.keypress("esc")
    out = sim.wait_for_exit(timeout=10)
    assert out.get("result") == "returned", out


def test_panels_logs_a_missing_image_once(simulator, test_sd_card):
    """The doc says logged and skipped. It was logged, and looked for on the
    card, every frame the panel was up."""
    from helpers import stage_lua_app
    _stage_panels_lib(test_sd_card)
    code = r"""
local Panels = picocalc.sys.loadlib("panels")
local comic = Panels.new({ name = "Missing", sequences = { { panels = {
    { frame = { height = 320 }, layers = { { image = "nope.png" } } },
} } } })
for i = 1, 40 do
    picocalc.input.update()
    comic:update()
    if comic.drewThisFrame ~= false then picocalc.display.flush() end
end
comic:teardown()
"""
    stage_lua_app(test_sd_card, "missing_img", code)
    simulator.launch_app("missing_img")
    out = simulator.wait_for_exit(timeout=20)
    assert out.get("result") == "returned", out
    lines = [t for t in _texts(simulator) if "image load failed" in t]
    assert len(lines) == 1, lines


def _doc_choices_snippet():
    text = (REPO_ROOT / "docs" / "Library-Panels.md").read_text()
    return text.split("### Choices", 1)[1].split("```lua\n", 1)[1].split("```", 1)[0]


def test_panels_validate_rejects_sequence_choices(simulator, test_sd_card):
    """`choices` belongs to a panel. On a sequence nothing ever read it, so the
    menu silently never opened: validate has to say so."""
    from helpers import stage_lua_app
    _stage_panels_lib(test_sd_card)
    stage_lua_app(test_sd_card, "seq_choices", r"""
local Panels = picocalc.sys.loadlib("panels")
local ok, err = pcall(Panels.validate, { sequences = {
    { panels = { { frame = { height = 320 } } },
      choices = { { text = "a", target = 1 } } },
} })
picocalc.sys.log("PT ok=" .. tostring(ok) .. " err=" .. tostring(err))
""")
    simulator.launch_app("seq_choices")
    assert simulator.wait_for_exit(timeout=10.0)["result"] == "returned"
    joined = "\n".join(_texts(simulator))
    assert ("PT ok=false err=panels: sequences[1]: "
            "choices belongs on a panel, not on a sequence") in joined, joined


def test_panels_doc_choices_snippet_opens_the_menu(sim_factory, test_sd_card):
    """The Choices section's snippet, as the page has it: it must pass validate
    and reach a menu, as the text above it says."""
    from helpers import stage_lua_app
    _stage_panels_lib(test_sd_card)
    # The snippet's targets are sequence 5, so give it a comic that has one.
    stage_lua_app(test_sd_card, "doc_choices", _doc_choices_snippet() + r"""
local Panels = picocalc.sys.loadlib("panels")
local plain = { panels = { { frame = { height = 320 } } } }
Panels.start({ sequences = { sequence, plain, plain, plain, plain } })
""")
    simulator = _paused_sim(sim_factory, test_sd_card)
    simulator.launch_app("doc_choices")
    _wait_for(simulator, "PANELS:SEQ 1")
    simulator.set_time_multiplier(0)
    _advance(simulator, TRANSITION_MS)   # any opening fade has finished
    _frame_barrier(simulator)            # the choice arms on a frame after it
    _tap(simulator, "enter")             # the first option: "Stay and fight"
    simulator.step_time(TRANSITION_MS)
    joined = _wait_for(simulator, "PANELS:SEQ 5")
    assert "PANELS:CHOICE 1 -> seq 5" in joined
    assert "PANELS:ERR" not in joined


@pytest.mark.parametrize("hw_scroll", ["true", "false"])
def test_panels_start_sleeps_on_idle_frames(sim_factory, test_sd_card, hw_scroll):
    """A comic waiting for a key must not spin, on either scroll path:
    Panels.start sleeps on an idle frame, and a key still gets through."""
    from helpers import stage_lua_app
    dest = test_sd_card / "system" / "lib"
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy(REPO_ROOT / "system" / "lib" / "panels.lua", dest / "panels.lua")
    stage_lua_app(test_sd_card, "panels_idle", r"""
local sys = picocalc.sys
local Panels = sys.loadlib("panels")
Panels.Settings.useHwScroll = """ + hw_scroll + r"""
local real_sleep, sleeps, updates = sys.sleep, 0, 0
sys.sleep = function(ms) sleeps = sleeps + 1; real_sleep(ms) end
local started = sys.getTimeMs()
local comic = { sequences = { { panels = { { frame = { height = 320 } } } } } }
local real_new = Panels.new
Panels.new = function(...)
    local c = real_new(...)
    local real_update = c.update
    c.update = function(self)
        updates = updates + 1
        if updates % 10 == 0 and sys.getTimeMs() - started > 1500 then
            sys.log(string.format("IDLE sleeps=%d updates=%d", sleeps, updates))
            started = sys.getTimeMs() + 100000
        end
        return real_update(self)
    end
    return c
end
print(Panels.start(comic))
""")
    simulator = sim_factory(test_sd_card)
    simulator.launch_app("panels_idle")
    joined = _wait_for(simulator, "IDLE sleeps=")
    m = re.search(r"IDLE sleeps=(\d+) updates=(\d+)", joined)
    sleeps, updates = int(m.group(1)), int(m.group(2))
    assert sleeps >= updates / 2, joined
    assert updates < 400, joined   # flat out it runs thousands of frames in 1.5 s
    _tap(simulator, "esc")
    _wait_for(simulator, "PANELS:EXIT quit")
    simulator.wait_for_exit(timeout=10)
