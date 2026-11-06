-- TCP resend and close on the device (tests/e2e/test_hw_tcp_resend.py), which
-- writes probe_in.json: {"mode": ..., "host": ..., "port": ..., ...}. The findings
-- go to probe_out.json. Modes:
--   tls_cycles    `cycles` times: TLS connect, write, optionally read the echo,
--                 close at once. PSRAM free and largest block after each, and
--                 every failure's text.
--   small_writes  `runs` times: 200 one-byte writes, the echo read back with a
--                 5 s limit: the pattern that stalled on a lost frame.
--   upload        `bytes` to a sink, 4 KB at a time with the documented retry.
--   write_close   one write of `bytes`, close at once.
--   dead_peer     connect, then a write a second until the socket fails.
local T = picocalc.sys.loadlib("picotest")
local pc = picocalc
local DIR = "/data/" .. APP_ID .. "/"
local cfg = pc.json.decode(pc.fs.readFile(DIR .. "probe_in.json"))
local now = pc.sys.getTimeMs

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "tcp_close_probe: " .. cfg.mode, pc.display.WHITE)
pc.display.flush()

local function write_all(s, data)
    local refused = 0
    while true do
        local n = s:write(data)
        if n >= 0 then return true, refused end
        if not s:isConnected() then return false, refused end
        refused = refused + 1
        pc.sys.sleep(5)
    end
end

local function write_file(name, text)
    local f = pc.fs.open(DIR .. name, "w")
    pc.fs.write(f, text)
    pc.fs.close(f)
end

local out = {}

local modes = {}

function modes.tls_cycles()
    local payload = string.rep("x", cfg.write or 1000)
    out.cycles = {}
    for i = 1, cfg.cycles do
        local t0 = now()
        local s = pc.tcp.new(cfg.host, cfg.port, true)
        local row = {ok = false}
        if not s then
            row.err = "tcp.new returned nil"
        else
            s:setInsecure(true)
            s:setConnectTimeout(20)
            s:connect()
            if s:waitConnected(25) then
                row.ok = write_all(s, payload)
                if row.ok and cfg.echo then
                    local got, deadline = 0, now() + 5000
                    while got < #payload and now() < deadline do
                        local d = s:read(4096)
                        if d then got = got + #d else pc.sys.sleep(5) end
                    end
                    row.ok = got == #payload
                end
            end
            if not row.ok then row.err = s:error() or "timed out" end
            s:close()
        end
        pc.sys.sleep(cfg.gap_ms or 0)
        local m = pc.sys.getMemInfo()
        row.free = m.psram_free
        row.largest = m.psram_largest_block
        row.ms = now() - t0
        out.cycles[i] = row
    end
end

function modes.small_writes()
    out.stalled, out.slow, out.max_ms = 0, 0, 0
    for run = 1, cfg.runs do
        local s = pc.tcp.new(cfg.host, cfg.port, false)
        s:connect()
        if not s:waitConnected(10) then
            out.connect_failed = (out.connect_failed or 0) + 1
        else
            local sent = {}
            for i = 1, 200 do
                local b = string.char(32 + i % 90)
                sent[i] = b
                write_all(s, b)
            end
            local t0, got, want = now(), {}, table.concat(sent)
            local len, deadline = 0, now() + 5000
            while len < #want and now() < deadline do
                local d = s:read(4096)
                if d then got[#got + 1] = d; len = len + #d else pc.sys.sleep(5) end
            end
            local ms = now() - t0
            if table.concat(got) ~= want then out.stalled = out.stalled + 1 end
            if ms > 500 then out.slow = out.slow + 1 end
            if ms > out.max_ms then out.max_ms = ms end
        end
        s:close()
    end
end

function modes.upload()
    local s = pc.tcp.new(cfg.host, cfg.port, false)
    s:connect()
    T.ok(s:waitConnected(10), "connect")
    local chunk = string.rep("u", 4096)
    local sent, refused, t0 = 0, 0, now()
    while sent < cfg.bytes do
        local ok, r = write_all(s, chunk)
        refused = refused + r
        if not ok then out.err = s:error(); break end
        sent = sent + #chunk
    end
    out.sent, out.refused, out.write_ms = sent, refused, now() - t0
    s:close()
end

function modes.write_close()
    local s = pc.tcp.new(cfg.host, cfg.port, false)
    s:connect()
    T.ok(s:waitConnected(10), "connect")
    local lines = {}
    for i = 1, cfg.bytes // 10 do lines[i] = string.format("%09d\n", i) end
    local data = table.concat(lines)
    T.eq(s:write(data), #data, "one large write")
    s:close()
    out.sent = #data
end

function modes.dead_peer()
    local s = pc.tcp.new(cfg.host, cfg.port, false)
    s:connect()
    T.ok(s:waitConnected(10), "connect")
    T.ok(write_all(s, "hello\n"), "first write")
    -- Ready: the test makes the host drop this device's frames now.
    write_file("probe_ready", "1")
    while not pc.fs.exists(DIR .. "probe_go") do pc.sys.sleep(50) end
    local t0 = now()
    while s:isConnected() and now() - t0 < cfg.limit_s * 1000 do
        s:write("x\n")
        pc.sys.sleep(1000)
    end
    out.connected_at_end = s:isConnected()
    out.err = s:error()
    out.after_ms = now() - t0
    s:close()
end

T.case(cfg.mode, function() modes[cfg.mode]() end)
write_file("probe_out.json", pc.json.encode(out))
T.done()
