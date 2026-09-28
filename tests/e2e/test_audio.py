"""Tests for audio subsystem via Lua app and RPC.

Verifies tone generation, volume control, and audio state reporting.
Uses get_audio_state RPC to check playback status.
"""


import pytest

from helpers import lua_case_names, run_lua_app


AUDIO_CASES = lua_case_names("audio_test")


@pytest.fixture(scope="module")
def audio_run(lua_suite):
    return lua_suite("audio_test")


class TestAudioLua:
    """audio_test (picotest kit): one pytest id per Lua case."""

    @pytest.mark.parametrize("case", AUDIO_CASES)
    def test_audio_case(self, audio_run, case):
        audio_run.check_case(case)

    def test_audio_suite_complete(self, audio_run):
        audio_run.assert_all_passed(AUDIO_CASES)


class TestAudioTeardown:
    """What an app leaves behind in the mixer does not reach the next app."""

    def test_exit_resets_the_master_volume(self, simulator):
        """audio_test leaves the master volume at 20; after it exits the
        volume is back to 100, so the next app (and its MP3) is not quiet
        until a reboot."""
        assert simulator.call("get_audio_state")["master_volume"] == 100
        run = run_lua_app(simulator, "audio_test")
        run.assert_all_passed(AUDIO_CASES)
        state = simulator.call("get_audio_state")
        assert state["master_volume"] == 100, state


class TestAudioRPC:
    """Test audio via direct RPC calls."""

    def test_play_tone_rpc(self, simulator):
        """Test play_tone RPC starts a tone."""
        result = simulator.call("play_tone", {
            "frequency": 440,
            "duration_ms": 2000,
        })
        assert result.get("ok"), f"play_tone failed: {result}"

        # Check audio state — tone should be playing (play_tone sets the
        # state before it replies; the 2 s tone is far from expiring)
        state = simulator.call("get_audio_state")
        assert state["tone_playing"] is True, (
            f"Tone should be playing after play_tone. State: {state}"
        )

        # Stop it
        simulator.call("stop_audio")

    def test_stop_audio_rpc(self, simulator):
        """Test stop_audio RPC stops playback."""
        # Start a tone
        simulator.call("play_tone", {"frequency": 880, "duration_ms": 5000})

        # Stop it
        result = simulator.call("stop_audio")
        assert result.get("ok"), f"stop_audio failed: {result}"

        # stop_audio clears the state before it replies.
        state = simulator.call("get_audio_state")
        assert state["tone_playing"] is False, (
            f"Tone should not be playing after stop_audio. State: {state}"
        )

    def test_audio_state_structure(self, simulator):
        """Test get_audio_state returns expected fields."""
        state = simulator.call("get_audio_state")
        assert "tone_playing" in state
        assert "stream_active" in state
        assert "sound_players_active" in state
        assert "master_volume" in state
        assert isinstance(state["tone_playing"], bool)
        assert isinstance(state["stream_active"], bool)
        assert isinstance(state["sound_players_active"], int)

    def test_idle_audio_state(self, simulator):
        """Test audio state when nothing is playing."""
        # Ensure nothing is playing
        simulator.call("stop_audio")

        state = simulator.call("get_audio_state")
        assert state["tone_playing"] is False
        assert state["stream_active"] is False
