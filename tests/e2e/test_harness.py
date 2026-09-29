"""Simulator test-control channel (audit R1/R11/R12).

Covers the RPCs and notifications the harness synchronises on:
- get_log_buffer {since_seq}: sequenced, sourced entries; paging; ring drop
- `log` notifications (subscribe {"logs": true})
- app.exited {name, id, found, result, error, runtime_ms}
- launch_app {"queued", "busy"}, rescan-on-miss, rescan_apps
- --test-mode: error text reaches the log at once, error screens don't block
- display_stats.present_count (wait_frames) and input seqs (get_input_state)
- boot parity (config_load at boot), SD-root containment, no dofile/loadfile
"""

import json
import socket
import threading
import time

import pytest

from helpers import stage_lua_app



@pytest.fixture
def harness_sim(sim_factory, test_sd_card):
    """Fresh --test-mode simulator on the per-test SD card.

    Writes /system/config.json before boot so boot-time config_load() is
    observable (harness_ok logs H:CFG <harness_key>).
    """
    (test_sd_card / "system" / "config.json").write_text(
        json.dumps({"harness_key": "42"}))
    return sim_factory(test_sd_card)


def _run(sim, name, timeout=15.0):
    sim.launch_app(name)
    return sim.wait_for_exit(timeout=timeout)


def _texts(entries, src=None):
    return [e["text"] for e in entries if src is None or e["src"] == src]


# ── Log channel ─────────────────────────────────────────────────────────────


def test_log_entries_are_sequenced_and_since_seq_is_honoured(harness_sim):
    sim = harness_sim
    _run(sim, "harness_ok")
    page = sim.get_log_buffer(0)
    lines = page["lines"]
    assert lines and all(isinstance(e, dict) for e in lines), page
    seqs = [e["seq"] for e in lines]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs), seqs
    assert all({"seq", "t_ms", "src", "text"} <= set(e) for e in lines)
    assert page["next_seq"] == seqs[-1] + 1
    assert page["dropped"] == 0

    app_lines = [e for e in lines if e["text"].startswith("H:")]
    assert all(e["src"] == "lua" for e in app_lines), app_lines
    line3 = next(e for e in lines if e["text"] == "H:LINE 3")

    newer = sim.get_log_buffer(since_seq=line3["seq"])["lines"]
    assert newer[0]["seq"] == line3["seq"]
    assert "H:LINE 2" not in _texts(newer) and "H:LINE 4" in _texts(newer)

    assert sim.get_log_buffer(since_seq=page["next_seq"])["lines"] == []

    tail = sim.get_log_buffer(tail=2)["lines"]
    assert [e["seq"] for e in tail] == seqs[-2:]

    # Launcher events arrive on the "os" source.
    assert any("start Harness OK" in t for t in _texts(lines, "os")), lines


def test_log_ring_keeps_newest_lines_and_reports_drops(harness_sim):
    sim = harness_sim
    _run(sim, "harness_flood", timeout=30.0)
    first = sim.get_log_buffer(0)
    assert first["dropped"] > 0, "5000 lines overflowed a 4096-line ring"
    entries = sim.get_log_lines(0)
    texts = _texts(entries)
    assert "H:FLOOD_DONE" in texts, texts[-5:]      # newest kept, not oldest
    assert "H:FLOOD 1 " + "x" * 200 not in texts
    assert len(entries) == 4096
    # Long lines survive whole (the old ring cut them at 256 chars).
    assert any(t.startswith("H:FLOOD 4999 ") and t.endswith("x" * 200) for t in texts)


def test_log_notifications_are_pushed_to_subscribers(harness_sim):
    sim = harness_sim
    assert sim.subscribe_logs(True) == {"logs": True}
    sim.launch_app("harness_ok")
    sim.wait_for_exit(timeout=15.0)
    deadline = time.time() + 2.0
    while time.time() < deadline:
        pushed = [e for e in list(sim._log_events) if e.get("text") == "H:DONE"]
        if pushed:
            break
        time.sleep(0.02)
    assert pushed and pushed[0]["src"] == "lua" and pushed[0]["seq"] > 0


# ── App outcome ─────────────────────────────────────────────────────────────


def test_app_exited_reports_returned(harness_sim):
    out = _run(harness_sim, "harness_ok")
    assert out["found"] is True
    assert out["result"] == "returned", out
    assert out["id"] == "net.picodeck.harness_ok"
    assert out["name"] == "harness_ok"
    assert out["error"] is None
    assert isinstance(out["runtime_ms"], int) and out["runtime_ms"] >= 0


def test_app_exited_reports_runtime_error_and_logs_it_at_once(harness_sim):
    sim = harness_sim
    sim.subscribe_logs(True)
    t0 = time.time()
    sim.launch_app("harness_err")
    text = sim.wait_for_log(r"harness boom 42", timeout=1.0, src="err")
    assert time.time() - t0 < 1.0
    assert "Runtime error:" in text
    sim.wait_for_log(r"stack traceback", timeout=1.0, src="err")

    out = sim.wait_for_exit(timeout=5.0)
    assert out["result"] == "error", out
    assert "harness boom 42" in out["error"]
    assert out["runtime_ms"] < 3000, "--test-mode must not wait on the error screen"


def test_app_exited_reports_exit_sentinel(harness_sim):
    out = _run(harness_sim, "harness_exit")
    assert out["result"] == "exit_sentinel", out
    texts = _texts(harness_sim.get_log_lines(0))
    assert "H:EXIT_START" in texts and "H:NOT_REACHED" not in texts


def test_unknown_app_reports_not_found(harness_sim):
    out = _run(harness_sim, "no_such_app_xyz")
    assert out["found"] is False
    assert out["result"] == "load_failed"
    assert "not found" in out["error"]


def test_min_psram_refusal_is_load_failed_and_logged(harness_sim):
    sim = harness_sim
    out = _run(sim, "harness_bigmem")
    assert out["result"] == "load_failed", out
    assert "contiguous" in out["error"]
    errs = _texts(sim.get_log_lines(0), "err")
    assert any("not enough PSRAM" in t for t in errs), errs
    assert "H:BIGMEM_RAN" not in _texts(sim.get_log_lines(0))


def _stage_app(sd, dirname, app_id, body):
    app = sd / "apps" / dirname
    app.mkdir()
    (app / "app.json").write_text(json.dumps({"id": app_id, "name": dirname}))
    (app / "main.lua").write_text(body)


def _tree(root):
    return sorted(str(p.relative_to(root)) for p in root.rglob("*")) \
        if root.exists() else []


# "com.victim." — FatFS strips trailing dots, so it would alias
# /data/com.victim (another app's data).
@pytest.mark.parametrize("bad_id", ["../evil", "a/b", "..", "has space", "",
                                    "com.victim.", "trailing."])
def test_invalid_app_id_is_refused_and_logged(harness_sim, test_sd_card, bad_id):
    """An app.json id is a path component (/data/<id>): the launcher refuses
    one that could escape it, with an on-screen reason, a load_failed outcome
    and an /system/error.log entry — the min_psram_kb refusal path."""
    sim = harness_sim
    data_before = _tree(test_sd_card / "data")
    _stage_app(test_sd_card, "evil_id", bad_id,
               'picocalc.sys.log("H:EVIL_RAN")\n')
    out = _run(sim, "evil_id")
    assert out["found"] is True and out["result"] == "load_failed", out
    assert "invalid app id" in out["error"], out
    errs = _texts(sim.get_log_lines(0), "err")
    assert any("invalid app id" in t for t in errs), errs
    assert "H:EVIL_RAN" not in _texts(sim.get_log_lines(0))
    log = (test_sd_card / "system" / "error.log").read_text()
    assert "invalid app id" in log and "evil_id" in log, log
    # Nothing was created outside /data for the bad id, nor under /data.
    assert not (test_sd_card / "evil").exists()
    assert _tree(test_sd_card / "data") == data_before


@pytest.mark.sd(fixtures=[], reserve=2)  # stages dup_a and dup_b
def test_apps_folding_to_one_id_are_warned_at_scan(harness_sim, test_sd_card):
    """Two apps whose ids fold to the same /data/<id> share a data dir and a
    config store: the scan logs a warning naming both."""
    sim = harness_sim
    _stage_app(test_sd_card, "dup_a", "com.test.dup", 'return\n')
    _stage_app(test_sd_card, "dup_b", "Com.Test.DUP", 'return\n')
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.rescan_apps()
    line = sim.wait_for_log(r"share id com\.test\.dup", timeout=10,
                            since_seq=seq)
    assert "dup_a" in line and "dup_b" in line, line


def test_app_id_is_folded_to_lower_case(harness_sim, test_sd_card):
    """FatFS is case-insensitive, so /data/<id> gets one spelling: an id with
    capitals is folded, and the app's store lands in the lower-case dir."""
    _stage_app(test_sd_card, "upper_id", "Com.Test.Upper",
               'local p = picocalc.fs.appPath("x")\n'
               'local f = picocalc.fs.open(p, "w")\n'
               'picocalc.fs.write(f, "ok") picocalc.fs.close(f)\n'
               'picocalc.sys.log("H:UPPER " .. p)\n')
    out = _run(harness_sim, "upper_id")
    assert out["result"] == "returned", out
    assert "H:UPPER /data/com.test.upper/x" in _texts(harness_sim.get_log_lines(0))
    assert (test_sd_card / "data" / "com.test.upper" / "x").read_text() == "ok"


# ── Launching ───────────────────────────────────────────────────────────────


def test_launch_app_staged_after_boot(harness_sim, test_sd_card):
    app = test_sd_card / "apps" / "harness_late"
    app.mkdir()
    (app / "app.json").write_text(json.dumps(
        {"id": "net.picodeck.harness_late", "name": "Harness Late"}))
    (app / "main.lua").write_text('picocalc.sys.log("H:LATE")\n')

    out = _run(harness_sim, "harness_late")
    assert out["found"] is True and out["result"] == "returned", out
    assert "H:LATE" in _texts(harness_sim.get_log_lines(0))


def test_rescan_apps_rescans_the_launcher(harness_sim):
    sim = harness_sim
    assert sim.rescan_apps() == {"queued": True}
    sim.wait_for_log(r"apps rescanned", timeout=2.0, src="os")


def test_launch_app_reports_queued_and_busy(harness_sim):
    sim = harness_sim
    r1 = sim.launch_app("harness_input")
    assert r1 == {"queued": True, "busy": False, "launch_id": r1["launch_id"]}
    sim.wait_for_log(r"^H:INPUT_READY$", timeout=5.0)
    # A second launch while the first app runs is queued behind it.
    r2 = sim.call("launch_app", {"name": "harness_ok"})
    assert r2 == {"queued": True, "busy": True, "launch_id": r1["launch_id"] + 1}
    sim.keypress("q")
    first = sim.wait_for_notification("app.exited", timeout=5.0)["params"]
    assert first["name"] == "harness_input" and first["result"] == "returned"
    assert first["launch_id"] == r1["launch_id"]
    second = sim.wait_for_notification("app.exited", timeout=5.0)["params"]
    assert second["name"] == "harness_ok" and second["result"] == "returned"
    assert second["launch_id"] == r2["launch_id"]


# ── Frame and input sync ────────────────────────────────────────────────────


def test_wait_frames_counts_presents(harness_sim):
    sim = harness_sim
    sim.launch_app("harness_input")
    sim.wait_for_log(r"^H:INPUT_READY$", timeout=5.0)
    before = sim.present_count()
    after = sim.wait_frames(5, timeout=3.0)
    assert after >= before + 5
    sim.keypress("q")
    sim.wait_for_exit(timeout=5.0)


def test_injections_return_seqs_that_get_consumed(harness_sim):
    sim = harness_sim
    sim.launch_app("harness_input")
    sim.wait_for_log(r"^H:INPUT_READY$", timeout=5.0)

    r1 = sim.keypress("a")
    r2 = sim.keypress("enter")
    assert r2["input_seq"] > r1["input_seq"] > 0
    state = sim.wait_input_consumed(r2["input_seq"], timeout=2.0)
    assert state["consumed_seq"] >= r2["input_seq"]
    assert state["issued_seq"] >= r2["input_seq"]
    sim.wait_for_log(r"^H:CHAR a$", timeout=2.0)
    sim.wait_for_log(r"^H:ENTER$", timeout=2.0)

    sim.keypress("q")
    sim.wait_for_exit(timeout=5.0)


# ── Boot parity and containment ─────────────────────────────────────────────


def test_config_is_loaded_at_boot(harness_sim):
    _run(harness_sim, "harness_ok")
    assert "H:CFG 42" in _texts(harness_sim.get_log_lines(0))


def test_dofile_and_loadfile_are_removed(harness_sim):
    _run(harness_sim, "harness_ok")
    assert "H:DOFILE nil nil" in _texts(harness_sim.get_log_lines(0))


def test_sd_escape_is_refused(harness_sim, test_sd_card):
    sim = harness_sim
    outside = test_sd_card.parent / "escape_probe.json"
    out = _run(sim, "harness_fs")
    assert out["result"] == "returned", out
    entries = sim.get_log_lines(0)
    assert not outside.exists(), "game.save wrote outside the SD root"
    # game.save refuses both names before any path is built (Task 6); the
    # simulator's SD-root containment itself is unit-tested (test_sim_sdcard).
    assert "H:ESCAPE false invalid save name" in _texts(entries)
    assert "H:INSIDE false invalid save name" in _texts(entries)
    assert not (test_sd_card / "saves" / "inside_probe.json").exists()
    assert not list(test_sd_card.rglob("*_probe.json"))


# ── Slow clients ────────────────────────────────────────────────────────────


def test_wait_for_log_backfills_dropped_notifications(harness_sim):
    """A client that falls behind loses `log` notifications (the server drops
    them whole once its 512 KB buffer is full); wait_for_log must still find
    a marker whose notification was dropped, by backfilling from the ring."""
    sim = harness_sim
    sim.subscribe_logs(True)
    sim.launch_app("harness_bigflood")
    sim.wait_for_log(r"^H:BIGFLOOD_READY$", timeout=5.0)

    # Start waiting before the marker exists, so only the notification path
    # (or a backfill) can find it.
    result = {}

    def waiter():
        try:
            result["text"] = sim.wait_for_log(r"^H:BIGFLOOD_DONE$", timeout=20.0)
        except Exception as e:  # reported below
            result["error"] = e

    t = threading.Thread(target=waiter)
    t.start()
    time.sleep(0.5)  # past wait_for_log's initial read of the ring

    # Stop reading, trigger ~18 MB of logs (more than the kernel buffers a
    # few MB plus the server's 512 KB), and stay paused until it is done.
    sim._reader_pause.set()
    try:
        # Raw send: call() would wait for a reply the paused reader can't see.
        sim._sock.sendall((json.dumps({"jsonrpc": "2.0", "id": 999999,
                                       "method": "inject_char",
                                       "params": {"char": "g"}}) + "\n").encode())
        # stdout is block-buffered, so the last lines may not show there:
        # wait for the flood's tail, then give it time to finish.
        deadline = time.time() + 20.0
        while "H:BIGFLOOD 1999" not in sim.get_output()["stdout"]:
            assert time.time() < deadline, "flood did not run"
            time.sleep(0.05)
        time.sleep(0.5)
    finally:
        sim._reader_pause.clear()

    t.join(timeout=25.0)
    assert result.get("text") == "H:BIGFLOOD_DONE", result
    # Prove the notification path really dropped entries.
    with sim._notif_cond:
        seqs = [e["seq"] for e in sim._log_events
                if e.get("text", "").startswith("H:BIGFLOOD ")]
    assert len(seqs) < 20000, "no notifications were dropped; test proves nothing"


def _raw_client(port):
    s = socket.create_connection(("127.0.0.1", port))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    return s


def test_stalled_client_is_disconnected_without_stalling_sys_log(harness_sim):
    sim = harness_sim
    sim.subscribe_logs(True)
    sim.launch_app("harness_ticker")
    sim.wait_for_log(r"^H:TICK 1$", timeout=5.0)

    # A second client asks for ~16 MB of screenshots and never reads.
    stalled = _raw_client(sim.tcp_port)
    reqs = "".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "screenshot",
                               "params": {"format": "raw"}}) + "\n"
                   for i in range(60))
    stalled.sendall(reqs.encode())

    # While the socket thread is stuck on it, ticks keep reaching client 1.
    arrivals = {}
    t_end = time.time() + 6.5
    while time.time() < t_end:
        with sim._notif_cond:
            for e in sim._log_events:
                if e.get("text", "").startswith("H:TICK ") and e["seq"] not in arrivals:
                    arrivals[e["seq"]] = time.time()
        time.sleep(0.02)
    times = sorted(arrivals.values())
    assert len(times) > 100, f"only {len(times)} ticks arrived during the stall"
    worst = max(b - a for a, b in zip(times, times[1:]))
    assert worst < 1.0, f"log delivery to client 1 stalled for {worst:.2f}s"

    # The stalled client is dropped (after the 5 s drain timeout): draining
    # its socket ends in EOF rather than an endless stream.
    stalled.settimeout(10.0)
    got_eof = False
    deadline = time.time() + 20.0
    while time.time() < deadline:
        try:
            chunk = stalled.recv(1 << 20)
        except ConnectionResetError:  # closed with our requests unread
            chunk = b""
        if not chunk:
            got_eof = True
            break
    stalled.close()
    assert got_eof, "stalled client was never disconnected"

    # sys.log itself never blocked: server-side tick timestamps stay dense.
    ticks = [e for e in sim.get_log_lines(0) if e["text"].startswith("H:TICK ")]
    t_ms = [e["t_ms"] for e in ticks]
    assert max(b - a for a, b in zip(t_ms, t_ms[1:])) < 500, "sys.log stalled"
    sim.wait_for_exit(timeout=15.0)


# ── Virtual time (--virtual-time, audit R14) and test-mode determinism ──────

VT_APP = """
local sys = picocalc.sys
local t0 = sys.getTimeMs()
sys.sleep(5000)
local t1 = sys.getTimeMs()
sys.log("VT:SLEPT " .. (t1 - t0))
"""

# 2026-01-01T00:00:00Z: the date --test-mode pins the clock to at boot.
TEST_EPOCH = 1767225600


@pytest.fixture
def vt_sim(sim_factory, test_sd_card):
    """A --test-mode --virtual-time simulator on the per-test SD card."""
    return sim_factory(test_sd_card, virtual_time=True)


def test_virtual_sleep_advances_the_clock_without_waiting(vt_sim, test_sd_card):
    stage_lua_app(test_sd_card, "vt_sleep", VT_APP)
    start = time.monotonic()
    vt_sim.launch_app("vt_sleep")
    text = vt_sim.wait_for_log(r"^VT:SLEPT \d+$", timeout=10.0)
    wall = time.monotonic() - start
    outcome = vt_sim.wait_for_exit(timeout=10.0)
    assert outcome["result"] == "returned", outcome
    slept = int(text.split()[1])
    # Only the sleep advances the clock: stretches without a sleep shorter
    # than the busy threshold (50 ms) add nothing.
    assert 5000 <= slept < 5100, slept
    assert wall < 2.5, f"sys.sleep(5000) took {wall:.2f}s of wall clock"


def test_real_time_test_mode_sleep_still_waits(sim_factory, test_sd_card):
    """Without --virtual-time the clock is the wall clock (default)."""
    sim = sim_factory(test_sd_card)
    stage_lua_app(test_sd_card, "vt_short", VT_APP.replace("5000", "400"))
    start = time.monotonic()
    sim.launch_app("vt_short")
    sim.wait_for_log(r"^VT:SLEPT \d+$", timeout=10.0)
    assert time.monotonic() - start >= 0.4


def test_step_time_advances_a_paused_clock(vt_sim, test_sd_card):
    sim = vt_sim
    stage_lua_app(test_sd_card, "vt_step", """
local sys = picocalc.sys
sys.log("VT:READY")
sys.sleep(1000)
sys.log("VT:WOKE")
""")
    sim.set_time_multiplier(0)
    base = sim.step_time(0)["now_ms"]
    time.sleep(0.2)  # wall time passes; the paused clock must not
    assert sim.step_time(0)["now_ms"] == base
    assert sim.step_time(250)["now_ms"] == base + 250

    sim.launch_app("vt_step")
    sim.wait_for_log(r"^VT:READY$", timeout=5.0)
    time.sleep(0.3)
    assert not [e for e in sim.get_log_lines(0) if e["text"] == "VT:WOKE"], \
        "sys.sleep returned while the clock was paused"
    sim.step_time(600)
    time.sleep(0.2)
    assert not [e for e in sim.get_log_lines(0) if e["text"] == "VT:WOKE"], \
        "sys.sleep(1000) returned after 600 ms of virtual time"
    sim.step_time(400)
    sim.wait_for_log(r"^VT:WOKE$", timeout=5.0)
    sim.set_time_multiplier(1)
    assert sim.wait_for_exit(timeout=5.0)["result"] == "returned"


def test_step_time_is_refused_on_the_wall_clock(harness_sim):
    with pytest.raises(Exception, match="virtual"):
        harness_sim.step_time(10)


DETERMINISM_APP = """
local sys = picocalc.sys
local r = {}
for i = 1, 8 do r[#r + 1] = tostring(math.random(1, 1000000)) end
sys.log("DET:RANDOM " .. table.concat(r, ","))
local t = {}
for i = 1, 40 do t["key" .. i] = i end
local order = {}
for k in pairs(t) do order[#order + 1] = k end
sys.log("DET:PAIRS " .. table.concat(order, ","))
local c = sys.getClock()
sys.log(string.format("DET:CLOCK %s %d %d %d %d", tostring(c.synced),
        c.epoch, c.hour, c.min, c.sec))
"""


def _determinism_run(sim_factory, sd):
    sim = sim_factory(sd)
    sim.launch_app("det_app")
    assert sim.wait_for_exit(timeout=10.0)["result"] == "returned"
    got = {}
    for e in sim.get_log_lines(0):
        if e["text"].startswith("DET:"):
            key, _, rest = e["text"].partition(" ")
            got[key] = rest
    sim.stop()
    return got


def test_math_random_and_table_order_repeat_across_runs(sim_factory, test_sd_card):
    stage_lua_app(test_sd_card, "det_app", DETERMINISM_APP)
    a = _determinism_run(sim_factory, test_sd_card)
    b = _determinism_run(sim_factory, test_sd_card)
    assert a["DET:RANDOM"] == b["DET:RANDOM"], (a, b)
    assert a["DET:PAIRS"] == b["DET:PAIRS"], (a, b)


def test_get_clock_is_pinned_to_a_fixed_date(sim_factory, test_sd_card):
    stage_lua_app(test_sd_card, "det_app", DETERMINISM_APP)
    got = _determinism_run(sim_factory, test_sd_card)
    synced, epoch, hour, minute, _sec = got["DET:CLOCK"].split()
    assert synced == "true", got
    # Pinned at boot, then running with the sim clock: same date, and a
    # fresh sim is still inside the first minutes of it.
    assert TEST_EPOCH <= int(epoch) < TEST_EPOCH + 600, got
    assert (int(hour), int(minute) < 10) == (0, True), got
