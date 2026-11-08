"""The App Store (apps/store) against a local catalog and release server
(issue #78).

The store reads its catalog from the `store_url` sysconfig key and release
assets from `store_download_base`, so nothing here needs the internet.

The simulator's WiFi is a mock: no SNTP clock gate, and its HTTP ignores a
disconnect. So these check the store's own side of #78: it redraws on input,
retries a catalog fetch that failed, waits for WiFi rather than giving up,
turns WiFi back on after an install, and stops retrying a failed download.
What only the device shows (TLS refused until the clock is set, a request on
a radio the store switched off) is in the issue.
"""

from __future__ import annotations

import collections
import hashlib
import io
import json
import threading
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

pytestmark = pytest.mark.sd(fixtures=[], extra=[("apps/store", "apps/store")])

MAX_RETRIES = 3          # apps/store/main.lua
UP = ("connected", "online")


def _app_zip() -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("app.json", json.dumps({
            "id": "net.picodeck.storetest", "name": "Store Test",
            "version": "1.0.0", "author": "test"}))
        z.writestr("main.lua", "return\n")
    return buf.getvalue()


APP_ZIP = _app_zip()


def _entry(name: str, dirname: str) -> dict:
    return {
        "id": f"net.picodeck.{dirname}", "dirname": dirname, "name": name,
        "description": f"{name} for the store tests", "version": "1.0.0",
        "author": "test", "category": "tools", "app_type": "lua",
        "min_firmware": "0.0.0", "size_kb": 1, "repo": f"test/{dirname}",
        "release_tag": "v1.0.0", "asset": f"{dirname}.zip",
        "sha256": hashlib.sha256(APP_ZIP).hexdigest(), "removable": True,
        "requirements": [],
    }


CATALOG = {
    "catalog_version": 1,
    "apps": [_entry("Store Test", "storetest"), _entry("Bravo", "bravo"),
             _entry("Charlie", "charlie")],
}


class StoreServer:
    """/catalog.json and /dl/<repo>/releases/download/<tag>/<asset>.

    catalog_failures: how many catalog requests to answer 503 before the
    catalog. asset_status: the status of every asset request (200 sends
    APP_ZIP). hits counts "catalog" and "asset" requests."""

    def __init__(self):
        self.catalog_failures = 0
        self.asset_status = 200
        self.hits: collections.Counter = collections.Counter()
        self._lock = threading.Lock()
        server = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def _send(self, status: int, body: bytes, ctype: str):
                self.send_response(status)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/catalog.json":
                    with server._lock:
                        server.hits["catalog"] += 1
                        fail = server.catalog_failures > 0
                        if fail:
                            server.catalog_failures -= 1
                    if fail:
                        self._send(503, b"busy", "text/plain")
                    else:
                        self._send(200, json.dumps(CATALOG).encode(),
                                   "application/json")
                elif self.path.startswith("/dl/"):
                    with server._lock:
                        server.hits["asset"] += 1
                    if server.asset_status == 200:
                        self._send(200, APP_ZIP, "application/zip")
                    else:
                        self._send(server.asset_status, b"no", "text/plain")
                else:
                    self._send(404, b"no", "text/plain")

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._httpd.daemon_threads = True
        self.url = f"http://127.0.0.1:{self._httpd.server_address[1]}"
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()

    def stop(self):
        self._httpd.shutdown()
        self._httpd.server_close()


@pytest.fixture
def store(sim_factory, test_sd_card):
    """(simulator, server, sd path): a fresh card (no cached catalog) whose
    system config points the store at a local server."""
    server = StoreServer()
    (test_sd_card / "system").mkdir(parents=True, exist_ok=True)
    (test_sd_card / "system" / "config.json").write_text(json.dumps({
        "wifi_ssid": "TestNet",
        "store_url": f"{server.url}/catalog.json",
        "store_download_base": f"{server.url}/dl",
    }))
    sim = sim_factory(test_sd_card, test_mode=True)
    yield sim, server, test_sd_card
    server.stop()


def _wait(cond, timeout: float, what: str):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return
        time.sleep(0.1)
    raise AssertionError(what)


def _settled(sim, timeout: float = 10.0) -> bytes:
    """A screenshot once two in a row, 300 ms apart, are the same."""
    deadline = time.time() + timeout
    prev = sim.screenshot()
    while time.time() < deadline:
        time.sleep(0.3)
        cur = sim.screenshot()
        if cur == prev:
            return cur
        prev = cur
    raise AssertionError("the screen never settled")


def _press(sim, key: str):
    sim.wait_input_consumed(sim.keypress(key)["input_seq"])


CATALOG_LOADED = rf"\[STORE\] Catalog: {len(CATALOG['apps'])} apps"


def _open_store(sim, server) -> bytes:
    """Launch the store and wait for its catalog to be on screen."""
    sim.launch_app("store")
    sim.wait_for_log(CATALOG_LOADED, timeout=20)
    return _settled(sim)


def _install_first_app(sim):
    _press(sim, "enter")   # Browse -> the first app's details
    _press(sim, "enter")   # install


def test_the_catalog_is_drawn_when_it_arrives(store):
    """The catalog's callback can run inside the loading screen's flush; the
    store must still draw the list, with no key pressed. F1 on the Browse tab
    changes nothing but redraws, so the screen must not change."""
    sim, server, _ = store
    first = _open_store(sim, server)
    _press(sim, "f1")
    assert _settled(sim) == first, \
        "the catalog arrived but the loading screen stayed up until a key"


def test_arrow_keys_move_the_selection_on_screen(store):
    sim, server, _ = store
    first = _open_store(sim, server)
    _press(sim, "down")
    second = _settled(sim)
    assert second != first, "Down moved the selection but the screen did not change"
    _press(sim, "up")
    assert _settled(sim) == first, "Up did not bring the first row back"


def test_a_failed_first_fetch_is_retried(store):
    """A fresh card has no cached catalog: one failed fetch (on the device,
    TLS refused until the clock is set) must not leave Browse empty."""
    sim, server, _ = store
    server.catalog_failures = 1
    sim.launch_app("store")
    _wait(lambda: server.hits["catalog"] >= 2, 20,
          "the store never asked for the catalog again after a failed fetch")
    sim.wait_for_log(r"\[STORE\] Catalog fetch failed: HTTP 503")
    sim.wait_for_log(CATALOG_LOADED, timeout=10)


def test_the_store_waits_for_wifi_to_come_up(store):
    """WiFi still joining at launch (the launcher starts the join for an
    "http" app): the store fetches once it is up."""
    sim, server, _ = store
    sim.call("set_wifi_state", {"status": "connecting"})
    sim.launch_app("store")
    time.sleep(2)
    assert server.hits["catalog"] == 0
    sim.call("set_wifi_state", {"status": "connected"})
    _wait(lambda: server.hits["catalog"] >= 1, 15,
          "the store did not fetch the catalog once WiFi was up")


def test_wifi_is_up_again_after_an_install(store):
    """The store drops WiFi to verify and unpack: the next download (another
    app, a firmware update) needs it back."""
    sim, server, sd = store
    _open_store(sim, server)
    _install_first_app(sim)
    _wait(lambda: (sd / "apps" / "storetest" / "main.lua").exists(), 20,
          "the app was not installed")
    _wait(lambda: sim.get_status()["wifi"]["status"] in UP, 10,
          f"WiFi stayed down after the install: {sim.get_status()['wifi']}")


def test_a_failing_download_gives_up_after_its_retries(store):
    sim, server, sd = store
    server.asset_status = 404
    _open_store(sim, server)
    _install_first_app(sim)
    _wait(lambda: server.hits["asset"] >= 1 + MAX_RETRIES, 40,
          f"only {server.hits['asset']} download attempts")
    time.sleep(8)
    assert server.hits["asset"] == 1 + MAX_RETRIES, \
        f"{server.hits['asset']} download attempts, expected 1 + {MAX_RETRIES}"
    assert not (sd / "apps" / "storetest").exists()
