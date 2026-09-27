local T = picocalc.sys.loadlib("picotest")
local sys, perf = picocalc.sys, picocalc.perf

T.case("getTimeUs_is_integer_and_advances", function()
    local a = sys.getTimeUs()
    T.eq(math.type(a), "integer")
    sys.sleep(20)
    local d = sys.getTimeUs() - a
    T.ok(d >= 19000 and d < 2000000, "20 ms sleep measured " .. d .. " us")
end)

T.case("wrap_arithmetic", function()
    -- A reading just before the 32-bit wrap and one just after: integer
    -- subtraction still gives the true gap.
    local before = math.maxinteger - 999      -- 2^31 - 1000
    local after = math.mininteger + 1000      -- wrapped
    T.eq(after - before, 2000)
end)

T.case("setTargetFPS_paces_to_the_deadline", function()
    perf.setTargetFPS(50)                     -- 20000 us per frame
    perf.beginFrame()
    perf.endFrame()                           -- first paced frame: no wait
    local t0 = sys.getTimeUs()
    for _ = 1, 10 do perf.endFrame() end      -- 10 frames of no work
    local d = sys.getTimeUs() - t0
    perf.setTargetFPS(0)
    T.ok(d >= 195000 and d < 2000000, "10 paced frames took " .. d .. " us")
end)

T.case("late_frame_resyncs_instead_of_sprinting", function()
    perf.setTargetFPS(50)
    perf.beginFrame(); perf.endFrame()
    sys.sleep(100)                            -- one 5-frame stall
    perf.endFrame()
    local t0 = sys.getTimeUs()
    perf.endFrame()                           -- must wait ~20 ms, not 0
    local d = sys.getTimeUs() - t0
    perf.setTargetFPS(0)
    T.ok(d >= 15000, "frame after a stall waited only " .. d .. " us")
end)

T.done()
