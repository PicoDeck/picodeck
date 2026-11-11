"""apps/store read from the gamepad, against test_store.py's local catalog
and release server (nothing needs the internet). The D-pad moves the
selection, L/R step the tabs (F2/F3, their default keys, step them too),
A opens and installs, X removes after a confirm that B declines and A
accepts, A on a Browse list with no catalog fetches it again, and B
leaves, even while the store waits for WiFi."""
import time

import pytest

from pad_apps import (answer_confirm, finish, launch, press, region,
                      set_wifi, tap, wait_region, wait_until)
from test_store import CATALOG_LOADED, _settled, store  # noqa: F401 (fixture)

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

TAB_BAR = (0, 21, 320, 42)   # ui.drawTabs below the 21-row header


def _open(sim):
    launch(sim, "store", settle=0)
    sim.wait_for_log(CATALOG_LOADED, timeout=20)
    return _settled(sim)


def test_store_waiting_for_wifi_leaves_on_b(simulator):
    sim = simulator
    set_wifi(sim, "connecting")
    launch(sim, "store")
    finish(sim)


def test_store_selection_and_tabs_on_the_pad(store):
    sim, _, _ = store
    first = _open(sim)
    press(sim, "down")
    assert _settled(sim) != first, "Down should move the highlight"
    press(sim, "up")
    assert _settled(sim) == first, "Up should bring the first row back"
    browse = region(sim, TAB_BAR)
    press(sim, "r")                                   # Browse -> Updates
    updates = wait_region(sim, TAB_BAR, lambda r: r != browse)
    press(sim, "r")                                   # Updates -> Installed
    installed = wait_region(sim, TAB_BAR, lambda r: r not in (browse, updates))
    press(sim, "l")                                   # back to Updates
    wait_region(sim, TAB_BAR, lambda r: r == updates)
    tap(sim, "f3")                                    # R's default key
    wait_region(sim, TAB_BAR, lambda r: r == installed)
    press(sim, "r")                                   # wraps to Browse
    wait_region(sim, TAB_BAR, lambda r: r == browse)
    finish(sim)


def test_store_install_and_remove_on_the_pad(store):
    sim, _, sd = store
    _open(sim)
    app = sd / "apps" / "storetest"
    press(sim, "a")              # Browse: the first app's details
    press(sim, "a")              # install
    wait_until(lambda: (app / "main.lua").exists(), timeout=20)
    press(sim, "b")              # details -> Browse
    press(sim, "r")
    press(sim, "r")              # Installed: "Store Test" sorts first
    press(sim, "x")              # remove...
    answer_confirm(sim, "b")     # ...no
    time.sleep(0.5)
    assert app.exists()
    press(sim, "x")              # remove...
    answer_confirm(sim, "a")     # ...yes
    wait_until(lambda: not app.exists())
    finish(sim)


def test_store_held_down_stops_on_the_last_app(store):
    """Down held in a three-app list repeats and stops on the last app:
    A then opens and installs that one."""
    sim, _, sd = store
    _open(sim)
    sim.pad("down")
    time.sleep(0.8)              # the first repeat at 200 ms, then every 80
    sim.pad("none")
    time.sleep(0.2)
    press(sim, "a")              # Charlie's details
    press(sim, "a")              # install
    wait_until(lambda: (sd / "apps" / "charlie" / "main.lua").exists(), timeout=20)
    assert not (sd / "apps" / "storetest").exists()
    press(sim, "b")              # details -> Browse
    finish(sim)


def test_store_a_on_an_empty_browse_list_fetches_again(store):
    """No catalog (every fetch fails): the empty list says why and A, like
    Enter, tries again at once instead of opening an app that is not
    there. The store's own retry waits 2 s after the first failure."""
    sim, server, _ = store
    server.catalog_failures = 100
    launch(sim, "store", settle=0)
    sim.wait_for_log(r"\[STORE\] Catalog fetch failed: HTTP 503", timeout=20)
    time.sleep(0.3)
    assert server.hits["catalog"] == 1
    press(sim, "a", gap=0)
    wait_until(lambda: server.hits["catalog"] >= 2, timeout=1.2)
    finish(sim)
