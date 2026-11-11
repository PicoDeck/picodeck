"""apps/updater read from the gamepad, offline: A starts the update check
(with WiFi pinned to "connecting" it stops at "WiFi not connected") and B
exits. The download and install screens need the network and a signed
release, so they are not driven here."""
import pytest

from pad_apps import finish, finish_key, launch, mark, press, set_wifi, tap

pytestmark = pytest.mark.sd(fixtures=[], reserve=1)

NOT_CONNECTED = r"^UPDATER: WiFi not connected$"


def test_updater_checks_on_a_and_exits_on_b(simulator):
    sim = simulator
    set_wifi(sim, "connecting")
    seq = mark(sim)
    launch(sim, "updater")
    press(sim, "a")
    sim.wait_for_log(NOT_CONNECTED, timeout=10, since_seq=seq)
    finish(sim)


def test_updater_enter_and_esc_still_work(simulator):
    sim = simulator
    set_wifi(sim, "connecting")
    seq = mark(sim)
    launch(sim, "updater")
    tap(sim, "enter")
    sim.wait_for_log(NOT_CONNECTED, timeout=10, since_seq=seq)
    finish_key(sim, "esc")
