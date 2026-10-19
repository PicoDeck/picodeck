-- TLS sockets against a local self-signed server (tests/e2e/test_tcp_tls.py):
-- setInsecure connects, the default refuses, and tcp:peerFingerprint() is
-- the server certificate's SHA-256. The harness writes
-- /data/<APP_ID>/servers.json: {"tls": port, "plain": port, "closing": port,
-- "fingerprint": hex, "second": hex}.
local pc = picocalc
local T = pc.sys.loadlib("picotest")
local cfg = pc.json.decode(pc.fs.readFile("/data/" .. APP_ID .. "/servers.json"))
local HOST = "127.0.0.1"

-- Waits until the socket has settled: connected, or failed with an error.
-- A loaded machine (parallel simulators) can take seconds over a handshake,
-- so this waits for the outcome, not a fixed time.
local function settle(s)
    local deadline = pc.sys.getTimeMs() + 20000
    while not s:isConnected() and not s:error() and pc.sys.getTimeMs() < deadline do
        pc.sys.sleep(20)
    end
    return s:isConnected()
end

-- A socket and whether it connected, once it has settled.
local function connect(port, tls, insecure)
    local s = T.ok(pc.tcp.new(HOST, port, tls), "tcp.new")
    if insecure then s:setInsecure(true) end
    s:setConnectTimeout(15)
    T.ok(s:connect(), "connect refused")
    return s, settle(s)
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

-- Gemini servers end every response with a TLS close_notify. The socket must
-- see that close and keep the fingerprint, and a reconnect on the same object
-- must report the new certificate, never the old one. The "closing" server
-- serves selfsigned.pem to its first connection and second.pem to the next.
T.case("a_server_close_keeps_the_fingerprint_and_a_reconnect_replaces_it", function()
    local s, ok = connect(cfg.closing, true, true)
    T.ok(ok, "not connected: " .. tostring(s:error()))
    T.eq(s:peerFingerprint(), cfg.fingerprint, "first connection")
    T.ok(s:write("bye\n") > 0, "write failed")
    local deadline = pc.sys.getTimeMs() + 10000
    while s:isConnected() and pc.sys.getTimeMs() < deadline do
        pc.sys.sleep(20)
    end
    T.ok(not s:isConnected(), "the server's close_notify was never seen")
    T.eq(s:peerFingerprint(), cfg.fingerprint, "after the server closed")
    T.ok(s:connect(), "reconnect refused")
    T.ok(settle(s), "not reconnected: " .. tostring(s:error()))
    T.eq(s:peerFingerprint(), cfg.second, "after reconnecting")
    s:close()
end)

T.done()
