"""json.encode's out-of-memory error must be a formatted message.

The message once used %u, which lua_pushfstring does not support, so running
out of memory inside the encoder raised "invalid option '%u' to
'lua_pushfstring'" instead of "json.encode: out of memory (N bytes)".
"""
import pytest

from helpers import stage_lua_app

APP = "json_oom_msg"

LUA = r"""
local pc = picocalc
local big = string.rep("x", 1024 * 1024)
local t = {}
for i = 1, 40 do t[i] = big end
local ok, e = pcall(pc.json.encode, t)
pc.sys.log("JO ok=" .. tostring(ok) .. " err=" .. tostring(e):sub(1, 80))
pc.sys.log("JO DONE")
"""


@pytest.mark.sd(fixtures=[], reserve=1)
def test_json_encode_oom_message(simulator, test_sd_card):
    stage_lua_app(test_sd_card, APP, LUA)
    simulator.launch_app(APP)
    simulator.wait_for_log("JO DONE", timeout=60.0)
    lines = [l if isinstance(l, str) else l.get("text", "")
             for l in simulator.get_log_buffer()["lines"]]
    res = [l for l in lines if "JO ok=" in l]
    assert res, lines[-10:]
    assert "invalid option" not in res[0], res[0]
    assert "ok=false" in res[0], res[0]
