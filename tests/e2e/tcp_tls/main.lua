-- TLS sockets against a local self-signed server (tests/e2e/test_tcp_tls.py):
-- setInsecure connects, the default refuses, and tcp:peerFingerprint() is
-- the server certificate's SHA-256. The harness writes
-- /data/<APP_ID>/servers.json: {"tls": port, "plain": port, "fingerprint": hex}.
local pc = picocalc
local T = pc.sys.loadlib("picotest")
local cfg = pc.json.decode(pc.fs.readFile("/data/" .. APP_ID .. "/servers.json"))
local HOST = "127.0.0.1"

-- A socket and whether it connected, once it has settled: connected, or
-- failed with an error. A loaded machine (parallel simulators) can take
-- seconds over a handshake, so this waits for the outcome, not a fixed time.
local function connect(port, tls, insecure)
    local s = T.ok(pc.tcp.new(HOST, port, tls), "tcp.new")
    if insecure then s:setInsecure(true) end
    s:setConnectTimeout(15)
    T.ok(s:connect(), "connect refused")
    local deadline = pc.sys.getTimeMs() + 20000
    while not s:isConnected() and not s:error() and pc.sys.getTimeMs() < deadline do
        pc.sys.sleep(20)
    end
    return s, s:isConnected()
end

T.case("insecure_tls_reports_the_fingerprint", function()
    local s, ok = connect(cfg.tls, true, true)
    T.ok(ok, "not connected: " .. tostring(s:error()))
    T.eq(s:peerFingerprint(), cfg.fingerprint)
    s:close()
    T.eq(s:peerFingerprint(), nil, "after close")
end)

T.case("verified_tls_refuses_a_self_signed_server", function()
    local s, ok = connect(cfg.tls, true, false)
    T.ok(not ok, "connected to a self-signed server with verification on")
    T.ok(s:error(), "no error")
    T.eq(s:peerFingerprint(), nil)
    s:close()
end)

T.case("plain_sockets_have_no_fingerprint", function()
    local s, ok = connect(cfg.plain, false, false)
    T.ok(ok, "not connected: " .. tostring(s:error()))
    T.eq(s:peerFingerprint(), nil)
    s:close()
end)

T.case("no_fingerprint_before_connecting", function()
    local s = T.ok(pc.tcp.new(HOST, cfg.tls, true), "tcp.new")
    T.eq(s:peerFingerprint(), nil)
    s:close()
end)

T.done()
