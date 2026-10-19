-- Firmware network stack cases (tests/e2e/test_network_firmware.py).
--
-- Runs against the simulator built with SIM_FIRMWARE_NET=ON, where
-- picocalc.network.http and picocalc.tcp are the firmware's http.c/tcp.c/
-- wifi.c on Mongoose (specs/test-audit-2026-09-24.md §3.6, R13).
--
-- The harness writes /data/<APP_ID>/servers.json before launch:
--   {"http": port, "echo": port, "blackhole": port, "big_sum": n,
--    "case": "<one case name>", "host": "<optional, default 127.0.0.1>"}
-- and this app runs ONLY that case, so a case that crashes the simulator
-- (the cross-core races this suite exists to reproduce) cannot take the
-- others down with it. Cases are registered with case_fw(<literal name>, fn);
-- the harness reads the names statically (helpers.lua_case_names).

local pc = picocalc
local net = pc.network
local T = pc.sys.loadlib("picotest")

local cfg = pc.json.decode(pc.fs.readFile("/data/" .. APP_ID .. "/servers.json"))
-- The simulator runs the servers on loopback; a hardware run names the
-- host (net_servers.py --advertise).
local HOST = cfg.host or "127.0.0.1"

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "net_fw: " .. tostring(cfg.case), pc.display.WHITE)
pc.display.flush()

local cases = {}
local order = {}
local function case_fw(name, fn)
    cases[name] = fn
    order[#order + 1] = name
end

-- ── Helpers ─────────────────────────────────────────────────────────────────

local function now() return pc.sys.getTimeMs() end

-- Wait (sleeping, which fires HTTP/TCP callbacks) until pred() is true.
local function wait(pred, timeout_ms)
    local deadline = now() + timeout_ms
    while not pred() do
        if now() >= deadline then return false end
        pc.sys.sleep(10)
    end
    return true
end

-- Spin in Lua without sleeping. The instruction hook (every 256 opcodes)
-- fires pending callbacks, so Core 0 reacts to a Core 1 event within
-- microseconds instead of the next 10 ms sleep slice: this makes Core 0's
-- free land between Core 1's poll cycles, which is the race window.
local function spin(ms, pred)
    local deadline = now() + ms
    local x = 0
    while now() < deadline do
        for i = 1, 200 do x = x + i end
        if pred and pred() then return true end
    end
    return pred and pred() or false
end

-- A GET/POST with every callback recorded. Returns the request table r:
-- r.conn, r.body (chunks read in the request callback), r.headers,
-- r.complete, r.closed, r.err, r.status.
local function request(path, opts)
    opts = opts or {}
    local r = { chunks = {}, headers = false, complete = false,
                closed = false }
    local conn = opts.conn or net.http.new(HOST, cfg.http, false, "e2e")
    T.ok(conn, "http.new returned nil")
    r.conn = conn
    -- Guarded anyway: a nested drain would append later bytes before the
    -- outer drain's chunk (callbacks no longer re-enter:
    -- http_callbacks_not_reentrant).
    local draining = false
    local function drain()
        if draining then return end
        draining = true
        while true do
            local s = conn:read(8192)
            if not s then break end
            r.chunks[#r.chunks + 1] = s
        end
        draining = false
    end
    r.drain = drain
    if opts.keepalive then conn:setKeepAlive(true) end
    if opts.connect_timeout then conn:setConnectTimeout(opts.connect_timeout) end
    if opts.read_timeout then conn:setReadTimeout(opts.read_timeout) end
    if opts.bufsize then T.ok(conn:setReadBufferSize(opts.bufsize), "bufsize") end
    conn:setHeadersReadCallback(function()
        r.headers = true
        r.status = conn:getResponseStatus()
    end)
    conn:setRequestCallback(function()
        if opts.on_data then opts.on_data(r) end
        if r.conn_closed_by_us then return end
        drain()
    end)
    conn:setRequestCompleteCallback(function()
        if not r.conn_closed_by_us then drain() end
        r.complete = true
        r.err = r.err or conn:getError()
    end)
    conn:setConnectionClosedCallback(function()
        r.closed = true
        r.err = r.err or conn:getError()
    end)
    local ok, why
    if opts.post then
        ok, why = conn:post(path, opts.post)
    else
        ok, why = conn:get(path)
    end
    T.ok(ok, "request refused: " .. tostring(why))
    return r
end

local function body(r) return table.concat(r.chunks) end

local function adler(s)
    local a, b = 1, 0
    for i = 1, #s do
        a = (a + s:byte(i)) % 65521
        b = (b + a) % 65521
    end
    return b * 65536 + a
end

-- The stack still works: a fresh GET /ok completes with the right body.
local function check_stack_alive(what)
    local r = request("/ok")
    T.ok(wait(function() return r.complete or r.closed end, 3000),
         (what or "") .. ": follow-up GET /ok never finished")
    T.eq(body(r), "hello picodeck", (what or "") .. ": follow-up body")
    r.conn:close()
end

local function tcp_open(port)
    local s = T.ok(pc.tcp.new(HOST, port or cfg.echo, false), "tcp.new")
    T.ok(s:connect(), "tcp connect refused")
    T.ok(s:waitConnected(3), "tcp never connected: " .. tostring(s:error()))
    return s
end

local function tcp_read_until(s, n, ms)
    local got = {}
    local len = 0
    wait(function()
        local d = s:read(4096)
        if d then got[#got + 1] = d; len = len + #d end
        return len >= n
    end, ms or 3000)
    return table.concat(got)
end

-- ── HTTP: expected to work ──────────────────────────────────────────────────

case_fw("http_get_ok", function()
    local r = request("/ok")
    T.ok(wait(function() return r.complete end, 3000),
         "complete never fired (closed=" .. tostring(r.closed) .. " err=" ..
         tostring(r.err) .. ")")
    T.eq(r.status, 200, "status")
    T.eq(body(r), "hello picodeck", "body")
    T.eq(r.err, nil, "error")
    r.conn:close()
end)

case_fw("http_get_big", function()
    local r = request("/big", { bufsize = 16384 })
    T.ok(wait(function() return r.complete or r.closed end, 15000),
         "1 MiB transfer did not finish")
    r.drain()
    local b = body(r)
    T.eq(#b, 1048576, "body length")
    T.eq(adler(b), cfg.big_sum, "body checksum")
    T.eq(r.err, nil, "error")
    r.conn:close()
end)

case_fw("http_post_text", function()
    local r = request("/echo", { post = "hello post" })
    T.ok(wait(function() return r.complete end, 3000), "POST never completed")
    T.eq(body(r), "hello post", "echoed body")
    r.conn:close()
end)

-- §3.6: conn:close() inside setRequestCallback.
case_fw("http_close_in_request_callback", function()
    local closed_at = nil
    local r
    r = request("/big", { on_data = function(rr)
        if not closed_at then
            closed_at = now()
            rr.conn_closed_by_us = true
            rr.conn:close()
        end
    end })
    T.ok(wait(function() return closed_at ~= nil end, 3000),
         "no data before close")
    spin(300)
    collectgarbage()
    collectgarbage()
    check_stack_alive("after close in callback")
end)

-- §3.6: close while /drip is in flight, then collect twice.
case_fw("http_close_during_drip_then_gc", function()
    local r = request("/drip")
    T.ok(wait(function() return #r.chunks >= 2 end, 3000), "no drip data")
    r.conn:close()
    collectgarbage()
    collectgarbage()
    spin(300)
    check_stack_alive("after close mid-drip")
end)

-- §3.6: drop the reference without close, then collect.
case_fw("http_drop_reference_then_gc", function()
    do
        local r = request("/drip")
        T.ok(wait(function() return #r.chunks >= 2 end, 3000), "no drip data")
    end
    collectgarbage()
    collectgarbage()
    spin(300)
    check_stack_alive("after GC of an in-flight request")
end)

-- Open/close churn: 20 complete request lifecycles on recycled slots.
case_fw("http_open_close_loop", function()
    for i = 1, 20 do
        local r = request("/ok")
        T.ok(wait(function() return r.complete or r.closed end, 3000),
             "iteration " .. i .. " never finished")
        T.eq(body(r), "hello picodeck", "iteration " .. i .. " body")
        r.conn:close()
    end
end)

-- §3.6 /reset: RST after the headers. Must end (closed or complete with an
-- error), not hang or crash.
case_fw("http_reset_after_headers", function()
    local r = request("/reset")
    T.ok(wait(function() return r.closed or r.complete end, 5000),
         "reset connection never reported")
    spin(300)
    check_stack_alive("after a reset")
end)

-- Exit with requests in flight (the next launch runs http_close_all).
case_fw("http_leave_inflight", function()
    _G.__keep = {}
    for i = 1, 4 do
        local r = request("/drip")
        _G.__keep[i] = r
    end
    T.ok(wait(function()
        for _, r in ipairs(_G.__keep) do
            if #r.chunks == 0 then return false end
        end
        return true
    end, 3000), "drips never started")
    -- return with all four open
end)

case_fw("http_after_inflight_exit", function()
    local rs = {}
    for i = 1, 4 do rs[i] = request("/ok") end
    T.ok(wait(function()
        for _, r in ipairs(rs) do
            if not (r.complete or r.closed) then return false end
        end
        return true
    end, 3000), "requests after the previous app's exit never finished")
    for i, r in ipairs(rs) do T.eq(body(r), "hello picodeck", "req " .. i) end
    for _, r in ipairs(rs) do r.conn:close() end
end)

-- ── HTTP: the code review's bugs (strict xfails until Task 13 fixed them) ──

-- A request callback must not run inside another one. sys.sleep fires the
-- pending callbacks with the instruction hook live, so the hook (every 256
-- opcodes) fires them again from inside a running callback.
case_fw("http_callbacks_not_reentrant", function()
    local depth, max_depth, calls = 0, 0, 0
    local conn = net.http.new(HOST, cfg.http, false, "reent")
    local done = false
    conn:setRequestCallback(function()
        depth = depth + 1
        calls = calls + 1
        if depth > max_depth then max_depth = depth end
        while conn:read(4096) do end
        spin(20)  -- let Core 1 queue more data while we are in here
        depth = depth - 1
    end)
    conn:setRequestCompleteCallback(function() done = true end)
    conn:setConnectionClosedCallback(function() done = true end)
    T.ok(conn:get("/big"), "get")
    T.ok(wait(function() return done end, 20000), "transfer never finished")
    T.eq(max_depth, 1, "request callback nesting depth (" .. calls .. " calls)")
    conn:close()
end)

-- The pool holds HTTP_MAX_CONNECTIONS (8); the 9th must fail cleanly.
case_fw("http_pool_exhaustion", function()
    local conns = {}
    for i = 1, 8 do
        conns[i] = net.http.new(HOST, cfg.http, false, "pool")
        T.ok(conns[i], "connection " .. i .. " of 8 refused")
    end
    local ninth = net.http.new(HOST, cfg.http, false, "pool")
    T.eq(ninth, nil, "9th connection")
    for _, c in ipairs(conns) do c:close() end
end)

-- §3.6: setConnectTimeout(1) to a black hole reports an error within 3 s.
-- Three rounds, spinning so Core 0 frees the slot before Core 1 delivers
-- the timed-out connection's MG_EV_CLOSE.
case_fw("http_connect_timeout", function()
    for round = 1, 3 do
        local r = request("/ok", { conn = net.http.new(HOST, cfg.blackhole,
                                                       false, "bh"),
                                   connect_timeout = 1 })
        local t0 = now()
        T.ok(spin(3000, function() return r.closed or r.complete end),
             "round " .. round .. ": no error within 3 s")
        T.ok(now() - t0 < 3000, "round " .. round .. ": took too long")
        T.ok(r.err and r.err:find("timeout"), "round " .. round ..
             ": error " .. tostring(r.err))
        spin(200)
    end
    check_stack_alive("after connect timeouts")
end)

-- §3.6 read timeout, before the headers: GET /hang with setReadTimeout(1),
-- then just wait. The Python side (test_read_timeout_fires_before_headers)
-- checks that the server saw the client hang up; the simulator's health is
-- deliberately not part of that verdict (http_read_timeout_mid_body owns
-- the crash that follows a timeout).
case_fw("http_read_timeout_hang", function()
    local r = request("/hang", { read_timeout = 1 })
    -- Long enough that the app's own exit (which closes the socket) cannot
    -- pass for a timeout inside the Python side's 2.5 s limit.
    wait(function() return false end, 4500)
end)

-- The read-timeout branch of http_check_timeouts, reached today: /stall
-- sends the headers and 10 of 1000 body bytes, then nothing (state BODY).
-- The timeout must be reported, and the stack must survive the teardown.
case_fw("http_read_timeout_mid_body", function()
    local r = request("/stall", { read_timeout = 1 })
    T.ok(spin(3000, function() return r.closed or r.complete end),
         "no read timeout within 3 s (err=" .. tostring(r.conn:getError()) .. ")")
    T.ok(r.err and r.err:find("read timeout"), "error " .. tostring(r.err))
    spin(200)
    check_stack_alive("after a read timeout")
end)

-- Keep-alive only: the connect timeout is long (10 s) so it cannot fire
-- inside the 3 s window, and the app's teardown closes the connection
-- through http_free's queued CLOSE; the timeout-path crash is not involved.
case_fw("http_keepalive_reuse", function()
    local conn = net.http.new(HOST, cfg.http, false, "ka")
    local r1 = request("/ok", { conn = conn, keepalive = true,
                                connect_timeout = 10 })
    T.ok(wait(function() return r1.complete end, 3000), "first request")
    T.eq(body(r1), "hello picodeck", "first body")
    local r2 = request("/ok", { conn = conn, keepalive = true,
                                connect_timeout = 10 })
    T.ok(wait(function() return r2.complete or r2.closed end, 3000),
         "second request on the kept-alive connection never finished")
    T.eq(r2.err, nil, "second request error")
    T.eq(body(r2), "hello picodeck", "second body")
    conn:close()
end)

case_fw("http_chunked_body", function()
    local r = request("/chunked")
    T.ok(wait(function() return r.complete end, 3000),
         "complete never fired for a chunked response (closed=" ..
         tostring(r.closed) .. ", body so far " .. string.format("%q", body(r)) .. ")")
    T.eq(body(r), "alphabetagamma", "de-chunked body")
    r.conn:close()
end)

case_fw("http_close_delimited_body", function()
    local r = request("/close")
    T.ok(wait(function() return r.complete end, 3000),
         "complete never fired for a close-delimited response (closed=" ..
         tostring(r.closed) .. ")")
    T.eq(body(r), "hello picodeck", "body")
end)

case_fw("http_post_binary", function()
    local payload = "a\0b\0c" .. string.rep("\0\1\2\3", 64) .. "end"
    local r = request("/echo", { post = payload })
    T.ok(wait(function() return r.complete end, 3000), "POST never completed")
    local b = body(r)
    T.eq(#b, #payload, "echoed length (server got " .. #b .. " bytes)")
    T.ok(b == payload, "echoed bytes differ")
    r.conn:close()
end)

-- ── TCP ─────────────────────────────────────────────────────────────────────

case_fw("tcp_echo", function()
    local s = tcp_open()
    T.eq(s:write("hello tcp"), 9, "write")
    T.eq(tcp_read_until(s, 9), "hello tcp", "echo")
    s:close()
    s:close()  -- double close is a no-op
end)

case_fw("tcp_read_after_close", function()
    local s = tcp_open()
    s:close()
    local ok = pcall(s.read, s)
    T.eq(ok, false, "read on a closed socket should raise")
    T.eq(s:available(), 0, "available after close")
    T.eq(s:isConnected(), false, "isConnected after close")
end)

case_fw("tcp_pool_exhaustion", function()
    local socks = {}
    for i = 1, 4 do
        socks[i] = pc.tcp.new(HOST, cfg.echo, false)
        T.ok(socks[i], "socket " .. i .. " of 4 refused")
    end
    T.eq(pc.tcp.new(HOST, cfg.echo, false), nil, "5th socket")
    for _, s in ipairs(socks) do s:close() end
end)

-- Leave sockets open and return: the app teardown (__gc → tcp_free) must
-- close them (the Python side checks the server sees the closes).
case_fw("tcp_leave_open", function()
    _G.__socks = {}
    for i = 1, 3 do
        local s = tcp_open()
        T.eq(s:write("x"), 1, "write")
        T.eq(tcp_read_until(s, 1), "x", "echo " .. i)
        _G.__socks[i] = s
    end
end)

-- Review Critical: tcp_free queues CLOSE and zeroes the slot at once, so
-- Core 1 never closes the Mongoose connection and its next event runs on
-- the dead slot. Close while the server is streaming at us.
case_fw("tcp_close_while_receiving", function()
    local s = tcp_open()
    s:write("FLOOD\n")
    T.ok(#tcp_read_until(s, 1024) > 0, "no flood data")
    s:close()
    spin(500)
    local s2 = tcp_open()
    s2:write("ping")
    T.eq(tcp_read_until(s2, 4), "ping", "echo after close")
    s2:close()
end)

-- Same race, observed as cross-talk: the freed slot is reused at once, so
-- the old connection's data lands in the new socket.
case_fw("tcp_close_then_reuse_slot", function()
    local a = tcp_open()
    a:write("FLOOD\n")
    T.ok(#tcp_read_until(a, 1024) > 0, "no flood data")
    a:close()
    local b = tcp_open()
    b:write("ping")
    spin(300)
    local got = tcp_read_until(b, 4, 1000)
    T.eq(got, "ping", "new socket received another connection's data")
    b:close()
end)

-- A request on a connection that is still busy is refused (it used to free
-- buffers Core 1 was reading and open a second connection on the slot).
case_fw("http_request_while_busy_refused", function()
    local r = request("/drip")
    local ok, why = r.conn:get("/ok")
    T.eq(ok, false, "second get on a busy connection")
    T.ok(why and why:find("in progress"), "reason " .. tostring(why))
    T.ok(wait(function() return #r.chunks >= 2 end, 3000),
         "the first request stopped")
    r.conn:close()
    check_stack_alive("after a refused request")
end)

-- The server closes a connection whose slot the app has already released
-- but Core 1 has not yet let go of (CLOSING): the CLOSE request is dropped
-- because the request ring is full (filled with TCP writes), and hooks do
-- not retry it while this callback runs.  MG_EV_CLOSE then arrives for the
-- CLOSING slot; the retried CLOSE must not touch the freed connection
-- (ASan: heap-use-after-free in http_c1_detach before the fix).
case_fw("http_server_closes_released_slot", function()
    local s = tcp_open()
    local conn = net.http.new(HOST, cfg.http, false, "rel")
    local done, ring_full = false, false
    conn:setRequestCallback(function()
        if done then return end
        done = true
        for _ = 1, 100 do
            if s:write("x") < 0 then ring_full = true; break end
        end
        conn:close()   -- CLOSE dropped: the ring is full
        spin(800)      -- the server closes at ~300 ms, slot still CLOSING
    end)
    T.ok(conn:get("/closelater"), "get")
    T.ok(wait(function() return done end, 3000), "no data")
    T.ok(ring_full, "the request ring never filled")
    spin(300)          -- the retried CLOSE reaches Core 1
    check_stack_alive("after a released slot's server closed")
    s:close()
end)

-- system/lib/download.lua end to end: 1 MiB to the app's data dir, with
-- the large (256 KB) ring it now asks for; size and checksum on disk.
case_fw("download_lib_big", function()
    local dl = pc.sys.loadlib("download")
    local dest = "/data/" .. APP_ID .. "/big.bin"
    local ok, err = dl.toFile("http://" .. HOST .. ":" .. cfg.http .. "/big",
                              dest, { timeoutMs = 20000 })
    T.ok(ok, "download failed: " .. tostring(err))
    local data = pc.fs.readFile(dest)
    T.eq(data and #data, 1048576, "file size")
    T.eq(adler(data), cfg.big_sum, "file checksum")
    pc.fs.delete(dest)
end)

-- A close-delimited body larger than the ring, not read until the server
-- has closed: what did not fit is kept (spilled), so the whole body is
-- still readable after COMPLETE.
case_fw("http_close_delimited_small_ring", function()
    local conn = net.http.new(HOST, cfg.http, false, "spill")
    T.ok(conn:setReadBufferSize(4), "bufsize")
    local complete, closed = false, false
    conn:setRequestCompleteCallback(function() complete = true end)
    conn:setConnectionClosedCallback(function() closed = true end)
    T.ok(conn:get("/close"), "get")
    T.ok(wait(function() return complete or closed end, 3000), "never finished")
    T.ok(complete, "not complete (closed=" .. tostring(closed) .. ", err=" ..
         tostring(conn:getError()) .. ")")
    local parts = {}
    while true do
        local d = conn:read(3)
        if not d then break end
        parts[#parts + 1] = d
    end
    T.eq(table.concat(parts), "hello picodeck", "the whole body after the close")
    conn:close()
end)

-- The link going down fails what is in flight at once (wifi_poll used to
-- stop polling and stop enforcing timeouts: the request hung forever), and
-- isHwDisconnected turns true once the link is really down.
case_fw("http_fails_on_link_down", function()
    local r = request("/drip")
    T.ok(wait(function() return #r.chunks >= 1 end, 3000), "no drip data")
    pc.wifi.disconnect()
    T.ok(wait(function() return r.closed end, 2000),
         "request still running 2 s after the link went down")
    T.ok(r.err and r.err:find("network down"), "error " .. tostring(r.err))
    T.ok(wait(function() return net.isHwDisconnected() end, 2000),
         "isHwDisconnected never turned true")
end)

-- TCP callbacks fire, with the socket as their argument (they used to be
-- dropped: tcp_lua_fire_pending took the events and discarded them).
case_fw("tcp_callbacks_fire", function()
    local s = T.ok(pc.tcp.new(HOST, cfg.echo, false), "tcp.new")
    local got = { connect = 0, read = 0, arg_ok = true, data = {} }
    s:setConnectCallback(function(c)
        got.connect = got.connect + 1
        if c ~= s then got.arg_ok = false end
    end)
    s:setReadCallback(function(c)
        got.read = got.read + 1
        if c ~= s then got.arg_ok = false end
        local d = c:read(4096)
        if d then got.data[#got.data + 1] = d end
    end)
    T.ok(s:connect(), "connect")
    T.ok(wait(function() return got.connect > 0 end, 3000),
         "connect callback never fired")
    T.eq(s:write("ping"), 4, "write")
    T.ok(wait(function() return #table.concat(got.data) >= 4 end, 3000),
         "read callback never delivered the echo")
    T.eq(table.concat(got.data), "ping", "echo via the read callback")
    T.ok(got.arg_ok, "callbacks get the socket")
    s:close()
end)

-- Events without a callback stay for sock:getEvents().
case_fw("tcp_get_events", function()
    local s = tcp_open()
    spin(50)  -- the hook fires pending callbacks meanwhile
    local ev = s:getEvents()
    T.ok(ev & pc.tcp.CB_CONNECT ~= 0, "getEvents lost CB_CONNECT (" .. ev .. ")")
    s:close()
end)

-- The server sends 20000 bytes (more than the 8 KiB ring) and goes quiet
-- while the app is not reading: the rest must still arrive once the app
-- reads (MG_EV_READ fires only for new bytes; the poll drain moves them).
case_fw("tcp_ring_refills_after_burst", function()
    local s = tcp_open()
    T.eq(s:write("BURST\n"), 6, "write")
    spin(300)
    local d = tcp_read_until(s, 20000, 3000)
    T.eq(#d, 20000, "bytes received after the burst")
    local want = {}
    for i = 0, 19999 do want[#want + 1] = string.char((i * 7) & 0xFF) end
    T.ok(d == table.concat(want), "burst bytes differ")
    s:close()
end)

-- The server sends 20000 bytes (more than the 8 KiB ring) and closes at once
-- while the app is not reading: everything must still be readable after the
-- close. Mongoose frees what it still holds after MG_EV_CLOSE, so tcp.c moves
-- it into the ring or a spill buffer first.
case_fw("tcp_data_survives_the_close", function()
    local s = tcp_open()
    T.eq(s:write("BURSTCLOSE\n"), 11, "write")
    spin(500)
    T.eq(s:isConnected(), false, "the server closed")
    local d = tcp_read_until(s, 20000, 3000)
    T.eq(#d, 20000, "bytes readable after the close")
    local want = {}
    for i = 0, 19999 do want[#want + 1] = string.char((i * 7) & 0xFF) end
    T.ok(d == table.concat(want), "bytes differ")
    T.eq(s:read(), nil, "nothing more")
    s:close()
end)

-- setReadTimeout reaches the connection: a connected socket that receives
-- nothing fails with "read timeout".
case_fw("tcp_read_timeout", function()
    local s = tcp_open()
    s:setReadTimeout(1)
    local t0 = now()
    T.ok(wait(function()
        local e = s:error()
        return e ~= nil and e:find("read timeout") ~= nil
    end, 3000), "no read timeout within 3 s: " .. tostring(s:error()))
    T.ok(now() - t0 >= 900, "timed out too early")
    T.eq(s:isConnected(), false, "isConnected after the timeout")
    s:close()
end)

-- Churn both pools (20 HTTP requests, 10 TCP echo round trips, each closed
-- right after), then fill them: all 8 HTTP and 4 TCP slots came back, and
-- one more of each is refused.  Slots are reclaimed asynchronously (Core 1
-- acknowledges each close); allocation waits briefly for that.
case_fw("pool_churn_then_fill", function()
    for i = 1, 20 do
        local r = request("/ok")
        T.ok(wait(function() return r.complete or r.closed end, 3000),
             "http " .. i .. " never finished")
        T.eq(body(r), "hello picodeck", "http " .. i .. " body")
        r.conn:close()
    end
    for i = 1, 10 do
        local s = tcp_open()
        T.eq(s:write("x"), 1, "tcp " .. i .. " write")
        T.eq(tcp_read_until(s, 1), "x", "tcp " .. i .. " echo")
        s:close()
    end
    local hs, ts = {}, {}
    for i = 1, 8 do
        hs[i] = net.http.new(HOST, cfg.http, false, "fill")
        T.ok(hs[i], "http slot " .. i .. " of 8 after the churn")
    end
    T.eq(net.http.new(HOST, cfg.http, false, "fill"), nil, "9th http")
    for i = 1, 4 do
        ts[i] = pc.tcp.new(HOST, cfg.echo, false)
        T.ok(ts[i], "tcp slot " .. i .. " of 4 after the churn")
    end
    T.eq(pc.tcp.new(HOST, cfg.echo, false), nil, "5th tcp")
    for _, c in ipairs(hs) do c:close() end
    for _, s in ipairs(ts) do s:close() end
end)

case_fw("tcp_connect_timeout", function()
    local s = T.ok(pc.tcp.new(HOST, cfg.blackhole, false), "tcp.new")
    s:setConnectTimeout(1)
    T.ok(s:connect(), "connect refused")
    local t0 = now()
    local failed = wait(function()
        local e = s:error()
        return e ~= nil and e:find("timeout") ~= nil
    end, 3000)
    T.ok(failed, "no connect timeout within 3 s (setConnectTimeout(1)); "
         .. "error: " .. tostring(s:error()))
    T.ok(now() - t0 < 3000, "took too long")
end)

-- ── Run the one selected case ───────────────────────────────────────────────

local fn = cases[cfg.case]
if not fn then
    T.case("unknown_case", function()
        T.fail("servers.json names no known case: " .. tostring(cfg.case))
    end)
else
    T.case(cfg.case, fn)
end
T.done()
