-- Run by test_ssh_transport.c, which sets up picocalc.crypto (the real bridge)
-- and check(cond, what), and either HANDSHAKES + SIGNATURES (recorded from
-- OpenSSH: ssh_handshake_vectors.h) or LIVE (a real sshd).

local wire = require("lib.ssh_wire")
local transport = require("lib.ssh_transport")
local auth = require("lib.ssh_auth")
local crypto = picocalc.crypto

-- A socket that replays bytes, and keeps what the client wrote. With peer_closed it
-- behaves as lua_bridge_tcp.c does after the peer's FIN: the data is still readable.
local function fake_conn(rx, peer_closed)
    local conn = {rx = rx or "", tx = ""}
    function conn:isConnected() return not peer_closed end
    function conn:read(max)
        if #self.rx == 0 then return nil end
        local out = self.rx:sub(1, max)
        self.rx = self.rx:sub(max + 1)
        return out
    end
    function conn:write(data)
        self.tx = self.tx .. data
        return #data
    end
    function conn:error() return nil end
    return conn
end

-- A transport that has reached the host key check of recorded handshake or signature v.
local function at_verify(v)
    local t = transport.new(fake_conn())
    t.hostkey_algo = v.hostkey_algo
    return t
end

local function flip(s, i)
    return s:sub(1, i - 1) .. string.char(s:byte(i) ~ 1) .. s:sub(i + 1)
end

local function namelist_has(names, wanted)
    for _, n in ipairs(names) do
        if n == wanted then return true end
    end
    return false
end

local function inner_sig(sig_blob)
    local _, pos = wire.read_string(sig_blob, 1)
    return (wire.read_string(sig_blob, pos))
end

-- ── Recorded handshakes ─────────────────────────────────────────────────────

local function test_recorded_signatures(v)
    local what = v.name .. ": "
    local t = at_verify(v)
    check(t:verify_host_key(v.K_S, v.H, v.sig_blob) == true, what .. "the real signature verifies")
    check(t.error == nil, what .. "no error after a good signature")

    -- The verifiers take the digest the host signed, SHA-256(H). The C verifier agrees, so
    -- passing H itself at the call site cannot pass this test.
    local verify = v.hostkey_algo == "rsa-sha2-256" and crypto.rsaVerify or crypto.ecdsaP256Verify
    local sig = inner_sig(v.sig_blob)
    check(verify(v.K_S, sig, crypto.sha256(v.H)) == true, what .. "C verifier takes SHA-256(H)")
    check(verify(v.K_S, sig, v.H) == false, what .. "C verifier refuses H itself")

    check(at_verify(v):verify_host_key(v.K_S, flip(v.H, 1), v.sig_blob) == false,
          what .. "another H fails")
    check(at_verify(v):verify_host_key(v.K_S, v.H, flip(v.sig_blob, #v.sig_blob)) == false,
          what .. "a changed signature fails")
    check(at_verify(v):verify_host_key(flip(v.K_S, #v.K_S), v.H, v.sig_blob) == false,
          what .. "another host key fails")
end

local function test_signature_algorithm_binding()
    local ec, rsa
    for _, v in ipairs(HANDSHAKES) do
        if v.hostkey_algo == "rsa-sha2-256" then rsa = v else ec = v end
    end

    -- A good signature, but not with the algorithm the exchange agreed on.
    local t = at_verify(rsa)
    t.hostkey_algo = "ecdsa-sha2-nistp256"
    check(t:verify_host_key(rsa.K_S, rsa.H, rsa.sig_blob) == false, "signature algorithm must be the negotiated one")
    check(t.error and t.error:find("rsa-sha2-256", 1, true), "the error names the signature algorithm")

    -- A key of the wrong type for the agreed algorithm: the C parsers skip the type field.
    t = at_verify(rsa)
    check(t:verify_host_key(ec.K_S, rsa.H, rsa.sig_blob) == false, "an ECDSA key cannot answer rsa-sha2-256")
    check(t.error and t.error:find("ecdsa-sha2-nistp256", 1, true), "the error names the key type")
    t = at_verify(ec)
    check(t:verify_host_key(rsa.K_S, ec.H, ec.sig_blob) == false, "an RSA key cannot answer ecdsa-sha2-nistp256")
    check(t.error and t.error:find("ssh-rsa", 1, true), "the error names the key type")

    -- ssh-rsa is SHA-1: never verified, even when the blob is otherwise a good one.
    local renamed = wire.string("ssh-rsa") .. wire.string(inner_sig(rsa.sig_blob))
    t = at_verify(rsa)
    check(t:verify_host_key(rsa.K_S, rsa.H, renamed) == false, "ssh-rsa signature refused when rsa-sha2-256 agreed")
    t = at_verify(rsa)
    t.hostkey_algo = "ssh-rsa"
    check(t:verify_host_key(rsa.K_S, rsa.H, renamed) == false, "ssh-rsa signature refused outright")

    -- Malformed blobs.
    for _, bad in ipairs({"", "\0\0\0", "\0\0\0\7ssh-rsa", "\255\255\255\255", rsa.sig_blob:sub(1, -2)}) do
        t = at_verify(rsa)
        check(t:verify_host_key(rsa.K_S, rsa.H, bad) == false, "malformed signature blob refused")
    end
    t = at_verify(rsa)
    check(t:verify_host_key("", rsa.H, rsa.sig_blob) == false, "empty host key refused")
    t = at_verify(rsa)
    check(t:verify_host_key(rsa.K_S:sub(1, 8), rsa.H, rsa.sig_blob) == false, "truncated host key refused")
end

local function test_exchange_hash(v)
    local t = transport.new(fake_conn())
    t.client_version, t.server_version = v.V_C, v.V_S
    t.client_kexinit_payload, t.server_kexinit_payload = v.I_C, v.I_S
    check(wire.mpint(v.shared) == v.K, v.name .. ": K encoded as an mpint")
    check(t:compute_exchange_hash(v.K_S, v.e, v.f, v.K) == v.H, v.name .. ": exchange hash")
end

local function test_negotiation(v)
    local t = transport.new(fake_conn())
    local server = t:parse_kexinit(v.I_S)
    check(t:negotiate_algorithms(server), v.name .. ": negotiates with sshd's KEXINIT")
    check(t.kex_algo == v.kex, v.name .. ": key exchange")
    check(t.hostkey_algo == v.hostkey_algo, v.name .. ": host key algorithm")
end

local function test_what_is_offered()
    local t = transport.new(fake_conn())
    local offer = t:parse_kexinit(t:build_kexinit())
    check(namelist_has(offer.hostkey, "rsa-sha2-256"), "rsa-sha2-256 offered")
    check(namelist_has(offer.hostkey, "ecdsa-sha2-nistp256"), "ecdsa-sha2-nistp256 offered")
    check(not namelist_has(offer.hostkey, "ssh-rsa"), "ssh-rsa (SHA-1) not offered")
    check(namelist_has(offer.kex, "curve25519-sha256"), "curve25519-sha256 offered")
    check(namelist_has(offer.kex, "ecdh-sha2-nistp256"), "ecdh-sha2-nistp256 offered")

    -- A server that can only do ssh-rsa, or a key type there is no verifier for.
    local server = t:parse_kexinit(t:build_kexinit())
    server.hostkey = {"ssh-rsa"}
    check(not t:negotiate_algorithms(server), "a server with only ssh-rsa is refused")
    check(t.error == "no common hostkey algorithm", "...as having no common algorithm")
end

local function test_key_derivation(v)
    local t = transport.new(fake_conn())
    t.cipher_algo, t.mac_algo = "aes128-ctr", "hmac-sha2-256"
    t.session_id, t._H, t._K_mpint = v.H, v.H, v.K
    local keys = t:derive_keys()
    for _, field in ipairs({"iv_c2s", "iv_s2c", "key_c2s", "key_s2c"}) do
        check(keys[field] == v[field], v.name .. ": " .. field)
    end
    check(keys.mackey_c2s == v.mac_c2s, v.name .. ": mac key c2s")
    check(keys.mackey_s2c == v.mac_s2c, v.name .. ": mac key s2c")
end

-- The first encrypted packet sshd sent decrypts and authenticates.
local function test_encrypted_reply(v)
    local t = transport.new(fake_conn(v.accept_wire))
    t.recv_cipher = crypto.aes_ctr_new(v.key_s2c, v.iv_s2c)
    t.recv_mac_key, t.recv_mac_algo = v.mac_s2c, "hmac-sha2-256"
    t.recv_seq = 3
    check(t:recv_packet(1000) == v.accept_payload, v.name .. ": decrypts sshd's SERVICE_ACCEPT")
    check(t.recv_seq == 4, v.name .. ": counts it")
    t:close()

    -- The sequence number is in the MAC.
    t = transport.new(fake_conn(v.accept_wire))
    t.recv_cipher = crypto.aes_ctr_new(v.key_s2c, v.iv_s2c)
    t.recv_mac_key, t.recv_mac_algo = v.mac_s2c, "hmac-sha2-256"
    t.recv_seq = 4
    check(t:recv_packet(1000) == nil, v.name .. ": a wrong sequence number fails the MAC")
    t:close()
end

-- ── Framing and encodings (RFC 4253 section 6) ──────────────────────────────

local function test_packet_framing()
    for _, block in ipairs({8, 16}) do
        for len = 0, 300 do
            local payload = string.rep("x", len)
            local packet = wire.build_packet(payload, block)
            local what = "payload " .. len .. ", block " .. block .. ": "
            check(#packet % block == 0, what .. "a whole number of blocks")
            check(#packet >= 16, what .. "at least 16 bytes")
            local packet_length = string.unpack(">I4", packet)
            local padding = packet:byte(5)
            check(packet_length == #packet - 4, what .. "packet_length")
            check(padding >= 4, what .. "at least 4 bytes of padding")
            check(padding == packet_length - len - 1, what .. "padding_length")
            check(packet:sub(6, 5 + len) == payload, what .. "payload intact")
        end
    end
end

local function test_mpint()
    check(wire.mpint("\1") == "\0\0\0\1\1", "mpint: small value")
    check(wire.mpint("\128") == "\0\0\0\2\0\128", "mpint: a sign byte when the top bit is set")
    check(wire.mpint("\0\0\127") == "\0\0\0\1\127", "mpint: leading zeros stripped")
    check(wire.mpint("\0\128") == "\0\0\0\2\0\128", "mpint: stripped, then a sign byte")
end

-- A server's reason reaches the user, including when it hung up straight after giving it.
local function test_disconnect_reason()
    local payload = string.char(transport.MSG.DISCONNECT) .. wire.uint32(3)
                 .. wire.string("key exchange refused") .. wire.string("")
    for _, peer_closed in ipairs({false, true}) do
        local t = transport.new(fake_conn(wire.build_packet(payload), peer_closed))
        check(t:recv_packet_skip(1000) == nil, "DISCONNECT ends the receive")
        check(t.error == "server disconnected: key exchange refused", "DISCONNECT reason: " .. tostring(t.error))
    end

    local t = transport.new(fake_conn("", true))
    check(t:recv_packet_skip(1000) == nil, "a closed socket with nothing left ends the receive")
    check(t.error and t.error:find("connection closed", 1, true), "reported as closed: " .. tostring(t.error))
end

-- ── A real sshd ─────────────────────────────────────────────────────────────

local function live_session(key, kex, algo)
    local what = kex .. " + " .. algo .. ": "
    local conn = LIVE.sshd_connect(key, kex, algo)
    local t = transport.new(conn)
    local ok = t:handshake()
    check(ok, what .. "handshake: " .. tostring(t.error))
    if ok then
        check(t.kex_algo == kex, what .. "key exchange")
        check(t.hostkey_algo == algo, what .. "host key algorithm")
        check(t.server_version:find("OpenSSH", 1, true), what .. "talking to OpenSSH")
        -- Encrypted both ways from here: sshd answers only what it could decrypt and authenticate.
        check(auth.request_service(t), what .. "SERVICE_REQUEST: " .. tostring(t.error))
        local auth_ok = auth.password(t, "picodeck-test", "not-a-password")
        check(auth_ok == false, what .. "a wrong password is refused")
        check(t.error and t.error:find("authentication failed", 1, true), what .. "as a failed login: " .. tostring(t.error))
    end
    t:close()
    conn:close()
end

local function live_refused(key, kex, algo, expect)
    local conn = LIVE.sshd_connect(key, kex, algo)
    local t = transport.new(conn)
    check(not t:handshake(), algo .. ": handshake refused")
    check(t.error == expect, algo .. ": " .. tostring(t.error))
    t:close()
    conn:close()
end

-- sshd says why it is hanging up, then closes at once.
local function live_disconnect(key, kex, algo)
    local conn = LIVE.sshd_connect(key, kex, algo, "MaxAuthTries=1")
    local t = transport.new(conn)
    check(t:handshake() and auth.request_service(t), "reaches authentication: " .. tostring(t.error))
    check(auth.password(t, "picodeck-test", "not-a-password") == false, "the login fails")
    check(t.error == "server disconnected: Too many authentication failures", "reason: " .. tostring(t.error))
    t:close()
    conn:close()
end

local function run_live()
    local ec_key = LIVE.keygen("ecdsa", "256")
    local rsa_key = LIVE.keygen("rsa", "3072")
    if not LIVE.sshd_usable(ec_key, "curve25519-sha256", "ecdsa-sha2-nistp256") then
        LIVE_SKIPPED = true
        return
    end
    for _, kex in ipairs({"curve25519-sha256", "ecdh-sha2-nistp256"}) do
        live_session(ec_key, kex, "ecdsa-sha2-nistp256")
        live_session(rsa_key, kex, "rsa-sha2-256")
    end
    live_disconnect(ec_key, "curve25519-sha256", "ecdsa-sha2-nistp256")
    if LIVE.sshd_usable(rsa_key, "curve25519-sha256", "ssh-rsa") then
        live_refused(rsa_key, "curve25519-sha256", "ssh-rsa", "no common hostkey algorithm")
    end
end

-- ── Run ─────────────────────────────────────────────────────────────────────

if LIVE then
    run_live()
else
    check(#HANDSHAKES == 4, "both key exchanges with both host key algorithms")
    for _, v in ipairs(HANDSHAKES) do
        test_recorded_signatures(v)
        test_exchange_hash(v)
        test_negotiation(v)
        test_key_derivation(v)
        test_encrypted_reply(v)
    end
    check(#SIGNATURES == 5, "the uncommon signature encodings")
    for _, v in ipairs(SIGNATURES) do
        test_recorded_signatures(v)
    end
    test_signature_algorithm_binding()
    test_what_is_offered()
    test_packet_framing()
    test_mpint()
    test_disconnect_reason()
end
