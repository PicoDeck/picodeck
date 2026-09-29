-- Perf Pacing Test: leaves the perf pacing target and frame history behind.
-- test_native.py::test_native_app_does_not_inherit_perf_pacing launches the
-- native API probe next and checks it starts from a clean perf state.
local perf = picocalc.perf

perf.setTargetFPS(5)
for _ = 1, 3 do
    perf.beginFrame()
    perf.endFrame()
end
picocalc.sys.log("PERF_LUA fps=" .. perf.getFPS())
-- Return without perf.setTargetFPS(0).
