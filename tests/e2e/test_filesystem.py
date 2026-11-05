"""Tests for filesystem operations via Lua app and RPC.

Regression targets:
- SD card flush on close/mkdir/delete/rename (commit 0965131)
- Multi-block write reliability (commit 6143878)
- Large file write/read roundtrip (exercises multi-block SD path)
"""

import base64
from pathlib import Path

import pytest

from helpers import lua_case_names, run_lua_app, stage_lua_app


FS_CASES = lua_case_names("fs_test")


@pytest.fixture(scope="module")
def fs_run(lua_suite):
    return lua_suite("fs_test")


class TestFilesystemLua:
    """fs_test (picotest kit, root-filesystem): one pytest id per Lua case."""

    @pytest.mark.parametrize("case", FS_CASES)
    def test_fs_case(self, fs_run, case):
        fs_run.check_case(case)

    def test_fs_suite_complete(self, fs_run):
        fs_run.assert_all_passed(FS_CASES)

    def test_fs_suite_is_idempotent(self, simulator):
        """fs_test passes on a second run against the same SD card (it used to
        FAIL exists/mkdir because its files persisted)."""
        for i in range(2):
            run_lua_app(simulator, "fs_test", timeout=15).assert_all_passed(FS_CASES)


COPY_EXIT_APP = """
local fs = picocalc.fs
local src, dst = fs.appPath("src.bin"), fs.appPath("dst.bin")
local f = fs.open(src, "w")
f:write(string.rep("x", 100000))
f:close()
local calls = 0
fs.copy(src, dst, function()
    calls = calls + 1
    picocalc.sys.log("COPY " .. calls)
    picocalc.sys.exit()
end)
picocalc.sys.log("NOT_REACHED")
"""


def test_exit_from_a_copy_progress_function_stops_the_copy(simulator):
    stage_lua_app(simulator.sd_card_path, "copy_exit", COPY_EXIT_APP,
                  id="com.test.copy_exit")
    seq = simulator.get_log_buffer(tail=1).get("next_seq", 0)
    simulator.launch_app("copy_exit")
    out = simulator.wait_for_exit(timeout=10)
    assert out.get("result") in ("returned", "exit_sentinel"), out
    texts = [e["text"] for e in simulator.get_log_lines(seq)]
    assert "COPY 1" in texts, texts
    assert "COPY 2" not in texts, texts
    assert "NOT_REACHED" not in texts, texts
    data = Path(simulator.sd_card_path) / "data" / "com.test.copy_exit"
    assert (data / "src.bin").exists()
    assert not (data / "dst.bin").exists(), "the partial copy stayed"


class TestFilesystemRPC:
    """Test filesystem operations via direct RPC calls."""

    def test_write_read_roundtrip(self, simulator):
        """Test file write/read roundtrip via RPC."""
        test_data = b"RPC write test: Hello from E2E!\x00\xff\x01"
        encoded = base64.b64encode(test_data).decode()

        result = simulator.call("write_file", {
            "path": "/data/rpc_test.bin",
            "data": encoded,
        })
        assert result.get("ok"), f"write_file failed: {result}"
        assert result.get("bytes_written") == len(test_data)

        # Read it back
        result = simulator.call("read_file", {"path": "/data/rpc_test.bin"})
        assert result.get("size") == len(test_data)
        got = base64.b64decode(result["data"])
        assert got == test_data, f"Data mismatch: {got!r} != {test_data!r}"

        # Clean up
        simulator.call("delete_file", {"path": "/data/rpc_test.bin"})

    def test_large_file_roundtrip(self, simulator):
        """Test 32KB file write/read (exercises multi-block writes).

        Regression test for SD card multi-block write reliability (commit 6143878).
        """
        # 32KB pattern — fits within the 256KB base64 decode buffer
        pattern = bytes(range(256)) * 128  # 32KB
        encoded = base64.b64encode(pattern).decode()

        result = simulator.call("write_file", {
            "path": "/data/rpc_large.bin",
            "data": encoded,
        }, timeout=30)
        assert result.get("ok"), f"write_file failed: {result}"

        result = simulator.call("read_file", {"path": "/data/rpc_large.bin"})
        got = base64.b64decode(result["data"])
        assert len(got) == len(pattern), f"Size mismatch: {len(got)} != {len(pattern)}"
        assert got == pattern, "Large file data corruption detected"

        simulator.call("delete_file", {"path": "/data/rpc_large.bin"})

    def test_list_dir(self, simulator):
        """Test directory listing via RPC."""
        result = simulator.call("list_dir", {"path": "/apps"})
        entries = result.get("entries", [])
        assert len(entries) > 0, "No entries in /apps"

        names = [e["name"] for e in entries]
        assert "hello" in names, f"hello app not found in {names}"

        for entry in entries:
            assert "name" in entry
            assert "is_dir" in entry

    def test_disk_info(self, simulator):
        """Test disk space reporting."""
        result = simulator.call("disk_info")
        assert "free_kb" in result
        assert "total_kb" in result
        assert result["total_kb"] > 0
        assert result["free_kb"] >= 0
        assert result["free_kb"] <= result["total_kb"]

    def test_file_create_and_delete(self, simulator):
        """Test file creation and deletion cycle."""
        simulator.call("write_file", {
            "path": "/data/delete_me.txt",
            "data": base64.b64encode(b"temp").decode(),
        })

        # Verify exists
        result = simulator.call("list_dir", {"path": "/data"})
        names = [e["name"] for e in result.get("entries", [])]
        assert "delete_me.txt" in names

        # Delete
        simulator.call("delete_file", {"path": "/data/delete_me.txt"})

        # Verify gone
        result = simulator.call("list_dir", {"path": "/data"})
        names = [e["name"] for e in result.get("entries", [])]
        assert "delete_me.txt" not in names

    def test_overwrite_file(self, simulator):
        """Test that writing to an existing file overwrites it."""
        path = "/data/overwrite_test.txt"

        # Write initial content
        simulator.call("write_file", {
            "path": path,
            "data": base64.b64encode(b"initial content here").decode(),
        })

        # Overwrite with shorter content
        simulator.call("write_file", {
            "path": path,
            "data": base64.b64encode(b"new").decode(),
        })

        result = simulator.call("read_file", {"path": path})
        got = base64.b64decode(result["data"])
        assert got == b"new", f"Expected b'new', got {got!r}"

        simulator.call("delete_file", {"path": path})
