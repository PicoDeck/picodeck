-- Run by test_ssh_transport.c, which sets up picocalc.crypto (the real bridge)
-- and check(cond, what), and either HANDSHAKES + SIGNATURES (recorded from
-- OpenSSH: ssh_handshake_vectors.h) or LIVE (a real sshd).

picocalc.input = {BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8, BTN_ENTER = 16, BTN_ESC = 32,
                  BTN_TAB = 64, BTN_BACKSPACE = 128, BTN_F1 = 256, BTN_F2 = 512, BTN_F3 = 1024,
                  BTN_F4 = 2048, BTN_F5 = 4096}

local wire = require("lib.ssh_wire")
local transport = require("lib.ssh_transport")
local auth = require("lib.ssh_auth")
local connection = require("lib.ssh_connection")
local keys = require("lib.ssh_input")
local crypto = picocalc.crypto

-- A socket that replays bytes, and keeps what the client wrote. With peer_closed it
-- behaves as lua_bridge_tcp.c does after the peer's FIN: the data is still readable.
local function fake_conn(rx, peer_closed)
    local conn = {rx = rx or "", tx = ""}
    conn.peer_closed = peer_closed
    function conn:isConnected() return not self.peer_closed end
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

-- A client transport past NEWKEYS, and a scripted server on the other end of its socket.
local function encrypted_session()
    local conn = fake_conn()
    local t = transport.new(conn)
    local key_c2s, iv_c2s = string.rep("c", 16), string.rep("C", 16)
    local key_s2c, iv_s2c = string.rep("s", 16), string.rep("S", 16)
    local mac_c2s, mac_s2c = string.rep("m", 32), string.rep("M", 32)
    t.session_id = string.rep("i", 32)
    t.send_cipher, t.send_mac_key, t.send_mac_algo = crypto.aes_ctr_new(key_c2s, iv_c2s), mac_c2s, "hmac-sha2-256"
    t.recv_cipher, t.recv_mac_key, t.recv_mac_algo = crypto.aes_ctr_new(key_s2c, iv_s2c), mac_s2c, "hmac-sha2-256"

    local server = {enc = crypto.aes_ctr_new(key_s2c, iv_s2c), dec = crypto.aes_ctr_new(key_c2s, iv_c2s),
                    seq_out = 0, seq_in = 0}
    -- Queue a packet whose framing the test controls: encrypted, with a good MAC.
    function server:send_raw(packet)
        local mac = crypto.hmacSHA256(mac_s2c, string.pack(">I4", self.seq_out) .. packet)
        self.seq_out = self.seq_out + 1
        return self.enc:update(packet) .. mac
    end
    function server:send(payload)
        conn.rx = conn.rx .. self:send_raw(wire.build_packet(payload, 16))
    end
    -- The payloads the client has sent since the last call.
    function server:received()
        local out = {}
        while #conn.tx > 0 do
            local first = self.dec:update(conn.tx:sub(1, 16))
            local length = string.unpack(">I4", first)
            local packet = first
            if length > 12 then packet = first .. self.dec:update(conn.tx:sub(17, length + 4)) end
            local mac = conn.tx:sub(length + 5, length + 36)
            check(mac == crypto.hmacSHA256(mac_c2s, string.pack(">I4", self.seq_in) .. packet), "client's MAC")
            self.seq_in = self.seq_in + 1
            out[#out + 1] = packet:sub(6, 4 + length - packet:byte(5))
            conn.tx = conn.tx:sub(length + 37)
        end
        return out
    end
    function server:close()
        self.enc:free()
        self.dec:free()
        t:close()
    end
    return t, server, conn
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

-- RFC 4253 section 7.1: each direction is negotiated on its own.
local function test_directions_negotiated_separately()
    local function server_lists(overrides)
        local lists = {
            kex = {"curve25519-sha256"}, hostkey = {"ecdsa-sha2-nistp256"},
            cipher_c2s = {"aes128-ctr"}, cipher_s2c = {"aes128-ctr"},
            mac_c2s = {"hmac-sha2-256"}, mac_s2c = {"hmac-sha2-256"},
            compress_c2s = {"none"}, compress_s2c = {"none"},
        }
        for k, v in pairs(overrides) do lists[k] = v end
        return lists
    end

    local conn = fake_conn(wire.build_packet(string.char(transport.MSG.NEWKEYS)))
    local t = transport.new(conn)
    check(t:negotiate_algorithms(server_lists({
        cipher_c2s = {"aes128-ctr"}, cipher_s2c = {"aes256-ctr"},
        mac_c2s = {"hmac-sha1"}, mac_s2c = {"hmac-sha2-256"},
    })), "different algorithms each way: " .. tostring(t.error))
    check(t.cipher_c2s == "aes128-ctr" and t.cipher_s2c == "aes256-ctr", "a cipher for each direction")
    check(t.mac_c2s == "hmac-sha1" and t.mac_s2c == "hmac-sha2-256", "a MAC for each direction")

    -- Key sizes follow each direction's algorithm.
    t.session_id, t._H, t._K_mpint = string.rep("h", 32), string.rep("h", 32), wire.mpint("\1\2\3")
    local keys = t:derive_keys()
    check(#keys.key_c2s == 16 and #keys.key_s2c == 32, "cipher key sizes")
    check(#keys.mackey_c2s == 20 and #keys.mackey_s2c == 32, "MAC key sizes")

    check(t:activate_keys(keys), "activates: " .. tostring(t.error))
    check(t.send_mac_algo == "hmac-sha1" and t.recv_mac_algo == "hmac-sha2-256", "MACs in the right directions")

    -- The client's messages and the server's use different algorithms from here on.
    local send = crypto.aes_ctr_new(keys.key_s2c, keys.iv_s2c)
    local packet = wire.build_packet("\99", 16)
    local mac = crypto.hmacSHA256(keys.mackey_s2c, string.pack(">I4", t.recv_seq) .. packet)
    conn.rx = send:update(packet) .. mac
    send:free()
    check(t:recv_packet_skip(300) == "\99", "a message from the server: " .. tostring(t.error))

    conn.tx = ""
    check(t:send_packet("\98"), "a message to the server")
    local dec = crypto.aes_ctr_new(keys.key_c2s, keys.iv_c2s)
    local sent = dec:update(conn.tx:sub(1, 16))
    dec:free()
    check(sent:sub(6, 6) == "\98", "encrypted with the client-to-server key")
    check(#conn.tx == 16 + 20, "with a hmac-sha1 tag")
    t:close()

    -- No match in either direction is a refusal, whichever it is.
    for _, case in ipairs({
        {{cipher_c2s = {"3des-cbc"}}, "no common cipher algorithm"},
        {{cipher_s2c = {"3des-cbc"}}, "no common cipher algorithm"},
        {{mac_c2s = {"hmac-md5"}}, "no common MAC algorithm"},
        {{mac_s2c = {"hmac-md5"}}, "no common MAC algorithm"},
        {{compress_c2s = {"zlib"}}, "no common compression algorithm"},
        {{compress_s2c = {"zlib"}}, "no common compression algorithm"},
    }) do
        local refused = transport.new(fake_conn())
        check(not refused:negotiate_algorithms(server_lists(case[1])), case[2] .. ": refused")
        check(refused.error == case[2], case[2] .. ": " .. tostring(refused.error))
    end
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
    t.cipher_c2s, t.cipher_s2c = "aes128-ctr", "aes128-ctr"
    t.mac_c2s, t.mac_s2c = "hmac-sha2-256", "hmac-sha2-256"
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
    check(t:recv_packet_skip(1000) == v.accept_payload, v.name .. ": decrypts sshd's SERVICE_ACCEPT")
    check(t.recv_seq == 4, v.name .. ": counts it")
    t:close()

    -- The sequence number is in the MAC.
    t = transport.new(fake_conn(v.accept_wire))
    t.recv_cipher = crypto.aes_ctr_new(v.key_s2c, v.iv_s2c)
    t.recv_mac_key, t.recv_mac_algo = v.mac_s2c, "hmac-sha2-256"
    t.recv_seq = 4
    check(t:recv_packet_skip(1000) == nil, v.name .. ": a wrong sequence number fails the MAC")
    t:close()
end

-- A packet that arrives in pieces, however long the gap, is still one packet.
local function test_packet_in_pieces(v)
    local wire_bytes = v.accept_wire
    for _, split in ipairs({1, 4, 15, 16, 17, #wire_bytes - 33, #wire_bytes - 32, #wire_bytes - 1}) do
        local conn = fake_conn(wire_bytes:sub(1, split))
        local t = transport.new(conn)
        t.recv_cipher = crypto.aes_ctr_new(v.key_s2c, v.iv_s2c)
        t.recv_mac_key, t.recv_mac_algo = v.mac_s2c, "hmac-sha2-256"
        t.recv_seq = 3
        check(t:recv_packet_skip(1) == nil, v.name .. ": split at " .. split .. ": not complete yet")
        conn.rx = wire_bytes:sub(split + 1)
        check(t:recv_packet_skip(1000) == v.accept_payload, v.name .. ": split at " .. split .. ": decrypts: " .. tostring(t.error))
        t:close()
    end
end

-- ── A hostile server's framing ──────────────────────────────────────────────

-- Must come back at once with a clear error: no Lua error, no waiting for bytes that will not come.
local function check_refused(what, t, expect)
    local start = picocalc.sys.getTimeMs()
    local ok, res = pcall(t.recv_packet_skip, t, 300)
    check(ok, what .. ": no Lua error: " .. tostring(res))
    check(ok and res == nil, what .. ": no packet")
    check(t.error and t.error:find(expect, 1, true), what .. ": " .. tostring(t.error))
    check(picocalc.sys.getTimeMs() - start < 250, what .. ": without waiting")
end

local BAD_LENGTHS = {
    {"\255\255\255\255", "2^32 - 1"},
    {"\128\0\0\0", "2^31, negative on the device"},
    {"\127\255\255\255", "2^31 - 1"},
    {"\0\1\0\0", "65536, over the RFC's 35000"},
    {"\0\0\0\0", "0"},
    {"\0\0\0\4", "4, under the minimum"},
    {"\0\0\0\13", "13, not a whole number of blocks"},
}

local function test_hostile_lengths()
    for _, bad in ipairs(BAD_LENGTHS) do
        local filler = string.rep("\7", 64)
        local t = transport.new(fake_conn(bad[1] .. filler))
        check_refused("plaintext length " .. bad[2], t, "bad packet length")

        -- Encrypted, a corrupt or hostile length must not be believed before the MAC either.
        local e, server, conn = encrypted_session()
        conn.rx = server.enc:update(bad[1] .. filler)
        check_refused("encrypted length " .. bad[2], e, "bad packet length")
        server:close()
    end
end

local function test_hostile_padding()
    -- length + padding_length + body: 16 bytes in all, so packet_length is 12.
    local function packet(padding)
        return "\0\0\0\12" .. string.char(padding) .. string.rep("\7", 11)
    end
    local cases = {
        {packet(255), "padding longer than the packet"},
        {packet(12), "padding of the whole packet"},
        {packet(11), "no payload"},
        {packet(3), "under 4 bytes of padding"},
    }
    for _, case in ipairs(cases) do
        check_refused("plaintext, " .. case[2], transport.new(fake_conn(case[1])), "bad padding")

        local e, server, conn = encrypted_session()
        conn.rx = server:send_raw(case[1])
        check_refused("encrypted, " .. case[2], e, "bad padding")
        server:close()
    end
end

-- The first error stays, and nothing more is taken from a stream that has gone wrong.
local function test_stream_stays_dead()
    local e, server, conn = encrypted_session()
    conn.rx = server:send_raw(wire.build_packet("\99", 16))
    conn.rx = conn.rx:sub(1, -2) .. string.char(conn.rx:byte(-1) ~ 1)
    check_refused("a bad MAC", e, "MAC verification failed")
    server:send("\99")
    check(e:recv_packet_skip(300) == nil, "nothing after a bad MAC")
    check(e.error == "MAC verification failed", "the first error stays: " .. tostring(e.error))
    server:close()
end

local function test_hostile_strings()
    -- A uint32 past 2^31 is negative on the device, and adding to one near it wraps.
    for _, len in ipairs({"\255\255\255\255", "\255\255\255\240", "\128\0\0\0", "\127\255\255\255"}) do
        check(wire.read_string(len .. "abc", 1) == nil, "read_string refuses a huge length")
        check(wire.read_namelist(len .. "abc", 1) == nil, "read_namelist refuses a huge length")
    end
    check(wire.read_string("\0\0\0\4abc", 1) == nil, "read_string: one past the end")
    check(wire.read_string("\0\0\0\3abc", 1) == "abc", "read_string: exact fit")
    check(wire.read_string("\0\0\0\0", 1) == "", "read_string: empty")
end

local function test_hostile_kexinit()
    local good = HANDSHAKES[1].I_S
    -- Cut inside each name-list, and a whole list too long.
    for _, cut in ipairs({1, 17, 18, 40, 100, #good - 60}) do
        local conn = fake_conn("SSH-2.0-hostile\r\n" .. wire.build_packet(good:sub(1, cut)))
        local t = transport.new(conn)
        local ok, res = pcall(t.handshake, t)
        check(ok, "KEXINIT cut at " .. cut .. ": no Lua error: " .. tostring(res))
        check(ok and res == false, "KEXINIT cut at " .. cut .. ": handshake fails")
        check(t.error == "malformed KEXINIT from server", "KEXINIT cut at " .. cut .. ": " .. tostring(t.error))
    end
end

local function test_hostile_banner()
    local t = transport.new(fake_conn(string.rep("x", 9000)))
    local start = picocalc.sys.getTimeMs()
    check(t:exchange_versions() == false, "a line that never ends")
    check(t.error and t.error:find("too long", 1, true), "reported: " .. tostring(t.error))
    check(picocalc.sys.getTimeMs() - start < 250, "without waiting")

    t = transport.new(fake_conn("", true))
    check(t:exchange_versions() == false, "no version string")
    check(t.error and t.error:find("connection closed", 1, true), "said as it is: " .. tostring(t.error))
end

-- The session loop: a channel on an encrypted connection, polled as main.lua does.
local function open_channel()
    local t, server, conn = encrypted_session()
    local ch = connection.new(t)
    ch.remote_channel, ch.remote_window = 7, 100000
    return ch, t, server, conn
end

local function channel_data(text)
    return string.char(94) .. wire.uint32(0) .. wire.string(text)
end

local function test_poll_packet_in_pieces()
    local ch, t, server, conn = open_channel()
    server:send(channel_data("hello"))
    local whole = conn.rx
    for _, split in ipairs({3, 16, 40}) do
        conn.rx = whole:sub(1, split)
        local start = picocalc.sys.getTimeMs()
        ch:poll(0)
        check(ch:read() == nil and not ch.closed, "split at " .. split .. ": waiting for the rest")
        check(picocalc.sys.getTimeMs() - start < 50, "split at " .. split .. ": poll(0) does not block")
        conn.rx = whole:sub(split + 1)
        ch:poll(0)
        check(ch:read() == "hello", "split at " .. split .. ": whole packet delivered")
        check(not ch.closed, "split at " .. split .. ": still open: " .. tostring(ch.error))
        server:send(channel_data("hello"))
        whole = conn.rx
    end
    server:close()
end

-- Everything already received is delivered before the session ends.
local function test_poll_ends_session()
    local ch, t, server, conn = open_channel()
    server:send(channel_data("last words"))
    conn.peer_closed = true
    ch:poll(0)
    check(ch:read() == "last words", "data before the close is delivered")
    ch:poll(0)
    check(ch.closed, "a closed socket ends the session")
    check(ch.error and ch.error:find("connection closed", 1, true), "and says why: " .. tostring(ch.error))
    server:close()

    ch, t, server, conn = open_channel()
    conn.rx = server:send_raw(wire.build_packet(channel_data("x"), 16))
    conn.rx = conn.rx:sub(1, -2) .. string.char(conn.rx:byte(-1) ~ 1)
    ch:poll(0)
    check(ch.closed, "a bad MAC ends the session")
    check(ch.error == "MAC verification failed", "and says why: " .. tostring(ch.error))
    server:close()

    ch, t, server, conn = open_channel()
    conn.rx = server.enc:update("\255\255\255\255" .. string.rep("\7", 60))
    local ok, err = pcall(ch.poll, ch, 0)
    check(ok, "a hostile length in the session: no Lua error: " .. tostring(err))
    check(ch.closed, "a hostile length ends the session")
    check(ch.error and ch.error:find("bad packet length", 1, true), "and says why: " .. tostring(ch.error))
    server:close()
end

-- ── Messages that can arrive at any time ────────────────────────────────────

local CHANNEL_SUCCESS = string.char(99)

local function global_request(name, want_reply)
    return string.char(80) .. wire.string(name) .. wire.boolean(want_reply)
end

-- What a server may send before the reply a caller is waiting for.
local NOT_FOR_THE_CALLER = {
    {string.char(2) .. wire.string("padding"), "IGNORE"},
    {string.char(3) .. wire.uint32(9), "UNIMPLEMENTED"},
    {string.char(4) .. wire.boolean(false) .. wire.string("note") .. wire.string(""), "DEBUG"},
    {string.char(7) .. wire.uint32(1) .. wire.string("server-sig-algs") .. wire.string("rsa-sha2-256"), "EXT_INFO"},
    {global_request("hostkeys-00@openssh.com", false), "GLOBAL_REQUEST"},
}

local function test_common_messages()
    for _, m in ipairs(NOT_FOR_THE_CALLER) do
        local t, server = encrypted_session()
        server:send(m[1])
        server:send(CHANNEL_SUCCESS)
        check(t:recv_packet_skip(300) == CHANNEL_SUCCESS, m[2] .. " is passed over while waiting: " .. tostring(t.error))
        check(#server:received() == 0, m[2] .. " needs no answer")
        server:close()

        local ch, _, session_server = open_channel()
        session_server:send(m[1])
        session_server:send(channel_data("after"))
        ch:poll(0)
        check(ch:read() == "after", m[2] .. " is passed over in the session")
        check(not ch.closed, m[2] .. " leaves the session open: " .. tostring(ch.error))
        check(#session_server:received() == 0, m[2] .. " needs no answer in the session")
        session_server:close()
    end
end

-- RFC 4254 section 4: a request that wants a reply gets one, in both paths.
local function test_global_request_reply()
    local t, server = encrypted_session()
    server:send(global_request("keepalive@openssh.com", true))
    server:send(CHANNEL_SUCCESS)
    check(t:recv_packet_skip(300) == CHANNEL_SUCCESS, "wanted a reply: still passed over")
    local sent = server:received()
    check(#sent == 1 and sent[1] == string.char(82), "wanted a reply: REQUEST_FAILURE")
    server:close()

    local ch, _, session_server = open_channel()
    session_server:send(global_request("keepalive@openssh.com", true))
    ch:poll(0)
    sent = session_server:received()
    check(#sent == 1 and sent[1] == string.char(82), "wanted a reply in the session: REQUEST_FAILURE")
    check(not ch.closed, "the session goes on: " .. tostring(ch.error))
    session_server:close()
end

local function check_rekey_refused(what, t, server)
    check(t.dead, what .. ": the connection ends")
    check(t.error and t.error:find("re-key", 1, true), what .. ": says why: " .. tostring(t.error))
    local sent = server:received()
    check(#sent == 1 and sent[1]:byte(1) == 1, what .. ": tells the server (DISCONNECT)")
end

-- The client cannot run a second key exchange; left alone, the server would wait for it for ever.
local function test_rekey_refused()
    local kexinit = HANDSHAKES[1].I_S
    local t, server = encrypted_session()
    server:send(kexinit)
    check(t:recv_packet_skip(300) == nil, "while waiting: no message for the caller")
    check_rekey_refused("while waiting", t, server)
    server:close()

    local ch, session_t, session_server = open_channel()
    session_server:send(channel_data("before"))
    session_server:send(kexinit)
    ch:poll(0)
    check(ch:read() == "before", "in the session: data before it is delivered")
    check(ch.closed, "in the session: it ends")
    check(ch.error and ch.error:find("re-key", 1, true), "in the session: says why: " .. tostring(ch.error))
    check_rekey_refused("in the session", session_t, session_server)
    session_server:close()

    -- The first KEXINIT is the start of the handshake, not a re-key.
    local plain = transport.new(fake_conn(wire.build_packet(kexinit)))
    check(plain:recv_packet_skip(300) == kexinit, "before keys, KEXINIT goes to the handshake")
    check(not plain.dead, "before keys, nothing is wrong")
end

-- RFC 4253 section 7.1: only key exchange messages before the keys, so these are errors there.
local function test_messages_before_keys()
    for _, m in ipairs({{global_request("x", true), 80}, {string.char(7), 7}}) do
        local conn = fake_conn(wire.build_packet(m[1]))
        local t = transport.new(conn)
        check(t:recv_packet_skip(300) == m[1], m[2] .. " before keys goes to the caller, who refuses it")
        check(#conn.tx == 0, m[2] .. " before keys is not answered")
    end
end

-- What the session loop sends back for channel messages it does not act on.
local function test_session_replies()
    -- RFC 4254 section 5.4: an unknown channel request is refused, not accepted.
    local ch, _, server = open_channel()
    server:send(string.char(98) .. wire.uint32(0) .. wire.string("keepalive@openssh.com") .. wire.boolean(true))
    ch:poll(0)
    local sent = server:received()
    check(#sent == 1 and sent[1] == string.char(100) .. wire.uint32(7), "unknown channel request: CHANNEL_FAILURE")

    server:send(string.char(98) .. wire.uint32(0) .. wire.string("exit-status") .. wire.boolean(false) .. wire.uint32(0))
    ch:poll(0)
    check(#server:received() == 0, "exit-status wants no reply")
    server:close()

    -- RFC 4254 section 5.1: a channel the server opens is refused.
    ch, _, server = open_channel()
    server:send(string.char(90) .. wire.string("x11") .. wire.uint32(5) .. wire.uint32(1024) .. wire.uint32(1024))
    ch:poll(0)
    sent = server:received()
    check(#sent == 1 and sent[1]:byte(1) == 92, "a channel the server opens: CHANNEL_OPEN_FAILURE")
    check(sent[1] and wire.read_uint32(sent[1], 2) == 5, "...naming the server's channel")
    check(not ch.closed, "...and the session goes on")
    server:close()

    -- RFC 4253 section 11.4: anything unknown gets UNIMPLEMENTED with its sequence number.
    ch, _, server = open_channel()
    server:send(channel_data("first"))
    server:send(string.char(200) .. "mystery")
    ch:poll(0)
    sent = server:received()
    check(#sent == 1 and sent[1] == string.char(3) .. wire.uint32(1), "an unknown message: UNIMPLEMENTED, its number")
    check(not ch.closed, "...and the session goes on")
    server:close()

    -- A late reply to a request is known, so it is not unimplemented.
    ch, _, server = open_channel()
    server:send(CHANNEL_SUCCESS .. wire.uint32(0))
    server:send(string.char(100) .. wire.uint32(0))
    ch:poll(0)
    check(#server:received() == 0, "a late channel reply needs no answer")
    server:close()
end

-- ── Opening a session ────────────────────────────────────────────────────────

local function channel_confirm(window)
    return string.char(91) .. wire.uint32(0) .. wire.uint32(7) .. string.pack(">I4", window) .. wire.uint32(32768)
end

local function window_adjust(n)
    return string.char(93) .. wire.uint32(0) .. string.pack(">I4", n)
end

local function new_channel()
    local t, server, conn = encrypted_session()
    return connection.new(t), t, server, conn
end

local function test_open_session()
    -- What OpenSSH does: a global request first, then the confirmation, with a window of 0.
    local ch, _, server = new_channel()
    server:send(global_request("hostkeys-00@openssh.com", false))
    server:send(channel_confirm(0))
    check(ch:open_session(), "opens past a global request: " .. tostring(ch.error))
    check(ch.remote_channel == 7, "the server's channel number")
    check(ch.remote_window == 0, "the server's window")
    server:close()

    -- The reason code, not where the reader stopped.
    ch, _, server = new_channel()
    server:send(string.char(92) .. wire.uint32(0) .. wire.uint32(2) .. wire.string("no sessions left") .. wire.string(""))
    check(not ch:open_session(), "a refused channel")
    check(ch.error == "channel open failed: no sessions left (code=2)", "says why: " .. tostring(ch.error))
    server:close()

    -- Nothing later can use a channel whose numbers were never read.
    for _, bad in ipairs({string.char(91), string.char(91) .. wire.uint32(0), string.char(91) .. wire.uint32(0) .. wire.uint32(7) .. wire.uint32(0)}) do
        ch, _, server = new_channel()
        server:send(bad)
        local ok, res = pcall(ch.open_session, ch)
        check(ok, "a short CHANNEL_OPEN_CONFIRM: no Lua error: " .. tostring(res))
        check(ok and res == false, "a short CHANNEL_OPEN_CONFIRM is refused")
        check(ch.error and ch.error:find("malformed", 1, true), "says why: " .. tostring(ch.error))
        server:close()
    end

    ch, _, server = new_channel()
    server:send(string.char(91) .. wire.uint32(9) .. wire.uint32(7) .. wire.uint32(0) .. wire.uint32(32768))
    check(not ch:open_session(), "a confirmation for another channel is refused")
    server:close()
end

-- Whatever comes before the reply is acted on, not mistaken for it.
local function test_requests_before_reply()
    for _, request in ipairs({"request_pty", "request_shell"}) do
        -- OpenSSH grants the window just before its reply to shell.
        local ch, _, server = new_channel()
        ch.remote_channel, ch.remote_window = 7, 0
        server:send(window_adjust(2097152))
        server:send(CHANNEL_SUCCESS .. wire.uint32(0))
        check(ch[request](ch), request .. ": past a window adjust: " .. tostring(ch.error))
        check(ch.remote_window == 2097152, request .. ": the whole window, was " .. ch.remote_window)
        server:close()

        -- Several things, and output first (nothing in the RFC orders them).
        ch, _, server = new_channel()
        ch.remote_channel, ch.remote_window = 7, 0
        server:send(window_adjust(100))
        server:send(channel_data("motd"))
        server:send(window_adjust(100))
        server:send(string.char(4) .. wire.boolean(false) .. wire.string("note") .. wire.string(""))
        server:send(CHANNEL_SUCCESS .. wire.uint32(0))
        check(ch[request](ch), request .. ": past output: " .. tostring(ch.error))
        check(ch.remote_window == 200, request .. ": both adjusts")
        check(ch:read() == "motd", request .. ": output kept")
        server:close()

        ch, _, server = new_channel()
        ch.remote_channel, ch.remote_window = 7, 0
        server:send(string.char(100) .. wire.uint32(0))
        check(not ch[request](ch), request .. ": CHANNEL_FAILURE is a refusal")
        server:close()

        ch, _, server = new_channel()
        ch.remote_channel, ch.remote_window = 7, 0
        server:send(string.char(97) .. wire.uint32(0))
        check(not ch[request](ch), request .. ": a closed channel is a refusal")
        check(ch.error and ch.error:find("closed", 1, true), request .. ": says why: " .. tostring(ch.error))
        server:close()
    end
end

-- The bug that let a shell open and then ignore all but the first 10 typed characters.
local function test_typing_after_shell()
    local ch, _, server = new_channel()
    ch.remote_channel, ch.remote_window = 7, 0
    server:send(window_adjust(2097152))
    server:send(CHANNEL_SUCCESS .. wire.uint32(0))
    check(ch:request_shell(), "the shell opens")
    server:received()
    local line = "echo typed more than ten characters\n"
    for i = 1, #line do
        check(ch:write(line:sub(i, i)), "typed character " .. i .. " is sent")
    end
    local typed = {}
    for _, payload in ipairs(server:received()) do
        check(payload:byte(1) == 94, "as CHANNEL_DATA")
        typed[#typed + 1] = wire.read_string(payload, 6)
    end
    check(table.concat(typed) == line, "the server receives the whole line")
    server:close()
end

local function test_close_unopened_channel()
    local ch, _, server = new_channel()
    local ok, err = pcall(ch.close, ch)
    check(ok, "closing a channel that never opened: no Lua error: " .. tostring(err))
    check(#server:received() == 0, "and nothing to send")
    server:close()
end

-- A window may be up to 2^32 - 1, and 2^31 and up are negative on the device.
local function test_large_windows()
    for _, window in ipairs({0xFFFFFFFF, 0x80000000, 0x7FFFFFFF}) do
        local ch, _, server = new_channel()
        server:send(channel_confirm(window))
        check(ch:open_session(), "opens with a huge window")
        check(ch.remote_window > 0, "a huge window is not negative: " .. ch.remote_window)
        check(ch:write("x"), "and can be written to")
        server:close()
    end

    local ch, _, server = open_channel()
    ch.remote_window = 0x7FFFFFFF
    server:send(window_adjust(1))
    server:send(window_adjust(0xFFFFFFFF))
    ch:poll(0)
    check(ch.remote_window > 0, "adding to a huge window does not wrap: " .. ch.remote_window)
    server:close()
end

-- ── How far the server can get ahead ─────────────────────────────────────────

-- The device's TCP layer fails a reader more than TCP_UNREAD_LIMIT bytes behind. Its
-- stack now offers the peer only what it has room for (TCP flow control), but the
-- channel window this client grants keeps a server under the limit too, as it did when
-- that was all there was. A server that uses all of it, into a reader that has stalled,
-- has to stay under the limit, framing included, and the credit has to go on coming as
-- the reader catches up.
local function test_window_stays_under_the_tcp_limit()
    for _, size in ipairs({"max", 4096, 1024, 256, 64}) do
        local what = "packets of " .. size .. ": "
        local ch, _, server, conn = new_channel()
        server:send(channel_confirm(0))
        check(ch:open_session(), what .. "opens: " .. tostring(ch.error))
        local open = server:received()[1]
        local _, pos = wire.read_string(open, 2)
        local _
        _, pos = wire.read_uint32(open, pos)  -- our channel
        local window
        window, pos = wire.read_uint32(open, pos)
        local max_packet = wire.read_uint32(open, pos)
        check(max_packet <= window, what .. "a packet fits the window")

        local chunk = math.min(size == "max" and max_packet or size, max_packet)
        local total, sent, got, worst = 256 * 1024, 0, 0, 0
        local credit = window
        while got < total do
            -- As the reader stalls, the server sends all it may.
            while credit > 0 and sent < total do
                local n = math.min(chunk, credit, total - sent)
                server:send(channel_data(string.rep("x", n)))
                credit, sent = credit - n, sent + n
            end
            worst = math.max(worst, #conn.rx)
            local before = got
            ch:poll(0)
            local data = ch:read()
            if data then got = got + #data end
            for _, msg in ipairs(server:received()) do
                check(msg:byte(1) == 93, what .. "only window adjusts come back")
                local _, at = wire.read_uint32(msg, 2)
                credit = credit + wire.read_uint32(msg, at)
            end
            if got == before and #conn.rx == 0 and credit == 0 then
                check(false, what .. "the window shut for good after " .. got .. " bytes")
                break
            end
        end
        check(worst <= TCP_UNREAD_LIMIT, what .. worst .. " bytes unread, over the limit of " .. TCP_UNREAD_LIMIT)
        check(got == total, what .. "every byte arrives: " .. got)
        check(not ch.closed, what .. "and the channel stays open: " .. tostring(ch.error))
        server:close()
    end
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

-- ── Keys ─────────────────────────────────────────────────────────────────────

-- What the keyboard gives getChar() (kbd_keycode_to_char) and what a terminal sends.
local function test_keys()
    check(keys.translate_char("\n") == "\r", "the Enter key sends CR")
    check(keys.translate_char("\8") == "\127", "the Backspace key sends DEL, the tty's erase")
    check(keys.translate_char("\1") == "\1", "Ctrl+A")
    check(keys.translate_char("\3") == "\3", "Ctrl+C")
    check(keys.translate_char("\26") == "\26", "Ctrl+Z")
    check(keys.translate_char("a") == "a", "a letter")
    check(keys.translate_char("~") == "~", "the last printable")
    check(keys.translate_char(nil) == nil, "no key")

    local inp = picocalc.input
    check(keys.translate_buttons(inp.BTN_UP, false) == "\27[A", "up, normal cursor keys")
    check(keys.translate_buttons(inp.BTN_UP, true) == "\27OA", "up, application cursor keys")
    check(keys.translate_buttons(inp.BTN_ESC, false) == "\27", "Esc")
end

-- Full-screen programs ask for application cursor keys, and with TERM=xterm-256color ncurses
-- then only knows ESC O A. tput smkx and rmkx print the first two of these.
local function test_cursor_key_mode()
    local cases = {
        {"\27[?1h\27=", false, true, "smkx"},
        {"\27[?1l\27>", true, false, "rmkx"},
        {"plain text", true, true, "text changes nothing"},
        {"\27[?1049h", false, false, "the alternate screen is not it"},
        {"\27[?25l", false, false, "nor is the cursor"},
        {"\27[?12h", false, false, "nor a mode that starts with 1"},
        {"\27[?25;1h", false, true, "in a list"},
        {"\27[?1;25l", true, false, "first in a list"},
        {"\27[?1hhello\27[?1l", false, false, "the last one wins"},
        {"\27[1h", false, false, "not a private mode"},
    }
    for _, c in ipairs(cases) do
        check(keys.track_cursor_keys(c[1], c[2]) == c[3], "cursor key mode: " .. c[4])
    end
end

-- ── The manifest ─────────────────────────────────────────────────────────────

-- The store (apps/store/main.lua, install_app) refuses an app whose min_firmware is
-- newer than the firmware it runs on. 0.6.0 is the first release with the overflow-safe
-- verifiers this app relies on (src/os/ssh_blob.c).
local function test_manifest()
    local root = package.path:match("^(.*)/apps/ssh/")
    local f = assert(io.open(root .. "/apps/ssh/app.json"))
    local manifest = f:read("a")
    f:close()
    local function version(text)
        local a, b, c = text:match("(%d+)%.(%d+)%.(%d+)")
        return tonumber(a) * 10000 + tonumber(b) * 100 + tonumber(c)
    end
    local min_firmware = manifest:match('"min_firmware"%s*:%s*"([^"]+)"')
    check(version(min_firmware) <= version("0.6.0"), "installable on the 0.6.0 release: min_firmware " .. min_firmware)
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
    local conn = LIVE.sshd_connect(key, kex, algo, {"MaxAuthTries=1"})
    local t = transport.new(conn)
    check(t:handshake() and auth.request_service(t), "reaches authentication: " .. tostring(t.error))
    check(auth.password(t, "picodeck-test", "not-a-password") == false, "the login fails")
    check(t.error == "server disconnected: Too many authentication failures", "reason: " .. tostring(t.error))
    t:close()
    conn:close()
end

-- ── A real shell ─────────────────────────────────────────────────────────────

-- The second field of a public key file: base64 of the key blob.
local function read_public_key(path)
    local f = assert(io.open(path))
    local b64 = f:read("a"):match("%S+%s+(%S+)")
    f:close()
    local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    local out, bits, nbits = {}, 0, 0
    for c in b64:gmatch("[^=]") do
        bits = (bits << 6) | (alphabet:find(c, 1, true) - 1)
        nbits = nbits + 6
        if nbits >= 8 then
            nbits = nbits - 8
            out[#out + 1] = string.char((bits >> nbits) & 0xFF)
            bits = bits & ((1 << nbits) - 1)
        end
    end
    return table.concat(out)
end

-- Test code, not in the app: an unprivileged sshd cannot check a password. RFC 4252 section 7.
local function publickey_login(t, client_pub)
    if not auth.request_service(t) then return false, t.error end
    local algo = "ecdsa-sha2-nistp256"
    local request = string.char(50) .. wire.string(LIVE.username()) .. wire.string("ssh-connection")
                 .. wire.string("publickey") .. wire.boolean(true) .. wire.string(algo)
                 .. wire.string(client_pub)
    local signature = LIVE.client_sign(wire.string(t.session_id) .. request)
    t:send_packet(request .. wire.string(wire.string(algo) .. wire.string(signature)))
    while true do
        local reply = t:recv_packet_skip(10000)
        if not reply then return false, t.error end
        local msg_type = reply:byte(1)
        if msg_type == 52 then return true end
        if msg_type == 51 then return false, "publickey refused" end
        if msg_type ~= 53 then return false, "unexpected message " .. msg_type end
    end
end

-- A shell without the user's own startup files, so nothing here depends on them.
local function login_options(extra)
    local options = {"AuthorizedKeysFile=" .. LIVE.client_pub_path, "StrictModes=no",
                     "PrintMotd=no", "PrintLastLog=no", "ForceCommand=/bin/sh"}
    for _, o in ipairs(extra or {}) do options[#options + 1] = o end
    return options
end

-- Log in as far as an open shell. Returns the channel, or nil after reporting why.
local function live_shell(what, key, kex, algo, options)
    local conn = LIVE.sshd_connect(key, kex, algo, login_options(options))
    local t = transport.new(conn)
    local ok, err = t:handshake(), t.error
    if ok then ok, err = publickey_login(t, LIVE.client_pub) end
    check(ok, what .. ": logs in: " .. tostring(err))
    if not ok then
        t:close()
        conn:close()
        return nil
    end
    return connection.new(t), t, conn
end

local function open_shell(what, ch)
    check(ch:open_session(), what .. ": session opens: " .. tostring(ch.error))
    if not ch.error then
        check(ch:request_pty("xterm-256color", 53, 26), what .. ": PTY: " .. tostring(ch.error))
    end
    if not ch.error then
        check(ch:request_shell(), what .. ": shell: " .. tostring(ch.error))
    end
    return not ch.error
end

-- As the app types: what the keyboard gives (\\n for Enter, \\8 for Backspace), through the
-- app's own key map, one packet per key.
local function type_keys(what, ch, typed)
    for i = 1, #typed do
        local sent = ch:write(keys.translate_char(typed:sub(i, i)))
        check(sent, what .. ": key " .. i .. " of " .. #typed .. " is sent, window " .. ch.remote_window)
    end
end

-- As the app reads: poll and take what there is. Returns all output once `text` is in it.
local function read_until(ch, text, timeout_ms)
    local output = ""
    local start = picocalc.sys.getTimeMs()
    while picocalc.sys.getTimeMs() - start < timeout_ms do
        ch:poll(0)
        local data = ch:read()
        if data then output = output .. data end
        if text and output:find(text, 1, true) then break end
        if ch.closed then break end
        picocalc.sys.sleep(2)
    end
    return output
end

-- Run a command with a reader that stalls for longer than the window lasts, draining the
-- socket as the TCP layer's ring would be read. Returns the most that was ever waiting.
local function live_stalled_reader(what, ch, t, conn, command)
    type_keys(what, ch, command .. "; echo PICO_DONE_$((40+2))\n")
    local output, worst = "", 0
    local start = picocalc.sys.getTimeMs()
    while picocalc.sys.getTimeMs() - start < 60000 do
        picocalc.sys.sleep(100)
        local waiting = 0
        local data = conn:read(8192)
        while data do
            waiting = waiting + #data
            t.recv_buf = t.recv_buf .. data
            data = conn:read(8192)
        end
        worst = math.max(worst, waiting)
        ch:poll(0)
        data = ch:read()
        if data then output = output .. data end
        if ch.closed or output:find("PICO_DONE_42", 1, true) then break end
    end
    check(output:find("PICO_DONE_42", 1, true), what .. ": it finished")
    check(not ch.closed, what .. ": still open: " .. tostring(ch.error))
    return worst
end

local function live_shell_session(key, kex, algo, options)
    local what = kex .. " + " .. algo .. " " .. table.concat(options or {}, " ")
    local ch, t, conn = live_shell(what, key, kex, algo, options)
    if not ch then return end

    if open_shell(what, ch) then
        -- The window OpenSSH grants is 2 MB; the bug that misread it left 10 bytes. The
        -- command's result is not in the echo of what was typed, so seeing it proves the
        -- shell received every key and ran the line.
        check(ch.remote_window > 1000, what .. ": the window is the server's, not " .. ch.remote_window)
        type_keys(what, ch, "echo PICO_$((40+2))\n")
        local output = read_until(ch, "PICO_42", 15000)
        check(output:find("PICO_42", 1, true), what .. ": the typed command ran: " .. output:sub(1, 200):gsub("%c", "."))

        -- A real tty's line editing: its erase character is what Backspace must send. The
        -- shell has read lines from it with `read` in canonical mode, unlike its own prompt.
        type_keys(what, ch, 'read x; echo "<$x>"\n')
        read_until(ch, "\n", 5000)
        type_keys(what, ch, "ab\8c\n")
        output = read_until(ch, "<ac>", 15000)
        check(output:find("<ac>", 1, true), what .. ": Backspace erases: " .. output:sub(1, 200):gsub("%c", "."))

        -- Several times the window this client grants, so it must top it up, and every
        -- byte must come through across packets and reads (the PTY sends \r\n).
        type_keys(what, ch, "seq 1 20000; echo PICO_DONE_$((40+2))\n")
        output = read_until(ch, "PICO_DONE_42", 30000)
        local expected = {}
        for i = 1, 20000 do expected[i] = tostring(i) end
        check(output:find(table.concat(expected, "\r\n") .. "\r\n", 1, true), what .. ": 20000 lines intact, " .. #output .. " bytes")
        check(output:find("PICO_DONE_42", 1, true), what .. ": and the shell is still there")
        check(not ch.closed, what .. ": still open: " .. tostring(ch.error))

        -- A real sshd against a stalled reader, held to what the device's TCP layer allows
        -- (test_window_stays_under_the_tcp_limit). Output in blocks is nearly all data.
        local worst = live_stalled_reader(what .. ": blocks", ch, t, conn, "head -c 150000 /dev/zero | tr '\\0' x")
        check(worst <= TCP_UNREAD_LIMIT, what .. ": blocks: " .. worst .. " bytes unread, over the limit of " .. TCP_UNREAD_LIMIT)
        -- A line at a time (seq on a tty) has as much framing as data, how much depending
        -- on how this machine's sshd and seq keep time: what chose the window, not checked.
        worst = live_stalled_reader(what .. ": lines", ch, t, conn, "seq 1 25000")
        print("NOTE test_ssh_transport --live: " .. what .. ": a line at a time, at most " .. worst .. " bytes unread of " .. TCP_UNREAD_LIMIT)

        type_keys(what, ch, "exit\n")
        read_until(ch, nil, 15000)
        check(ch.closed, what .. ": exit closes the channel")
        check(ch.error == nil, what .. ": a clean end: " .. tostring(ch.error))
    end
    ch:close()
    t:close()
    conn:close()
end

-- sshd's own keepalives want a reply and end the connection without one: a global request
-- while no channel is open, a channel request once one is. With these settings the second
-- one unanswered, at 2 seconds, is the end.
local function live_keepalives(key, kex, algo)
    local what = "keepalives"
    local ch, t, conn = live_shell(what, key, kex, algo, {"ClientAliveInterval=1", "ClientAliveCountMax=1"})
    if not ch then return end

    check(t:recv_packet_skip(3000) == nil and not t.dead, what .. ": alive with no channel: " .. tostring(t.error))
    if not t.dead and open_shell(what, ch) then
        read_until(ch, nil, 3000)
        check(not ch.closed, what .. ": alive in a session: " .. tostring(ch.error))
        type_keys(what, ch, "echo PICO_$((40+2))\n")
        check(read_until(ch, "PICO_42", 15000):find("PICO_42", 1, true), what .. ": the shell still answers")
    end
    ch:close()
    t:close()
    conn:close()
end

-- A real re-key, mid-session. A server sends nothing more until it is done, so this must end
-- the session with a reason rather than leave it silently frozen.
local function live_rekey(key, kex, algo)
    local what = "re-key"
    local ch, t, conn = live_shell(what, key, kex, algo, {"RekeyLimit=64K"})
    if not ch then return end

    if open_shell(what, ch) then
        type_keys(what, ch, "seq 1 20000\n")
        read_until(ch, nil, 30000)
        check(ch.closed, what .. ": ends the session")
        check(ch.error and ch.error:find("re-key", 1, true), what .. ": and says why: " .. tostring(ch.error))
    end
    ch:close()
    t:close()
    conn:close()
end

local function run_live_shell(ec_key, rsa_key)
    if not LIVE.has_pty() then
        print("NOTE test_ssh_transport --live: no pseudo-terminals here, so no shell tests")
        return
    end
    LIVE.client_pub_path = LIVE.client_key()
    LIVE.client_pub = read_public_key(LIVE.client_pub_path)

    live_shell_session(ec_key, "curve25519-sha256", "ecdsa-sha2-nistp256")
    live_shell_session(rsa_key, "ecdh-sha2-nistp256", "rsa-sha2-256")
    -- The other cipher and MAC the client offers, which sshd would not otherwise pick.
    live_shell_session(rsa_key, "curve25519-sha256", "rsa-sha2-256", {"Ciphers=aes256-ctr", "MACs=hmac-sha1"})
    live_keepalives(ec_key, "curve25519-sha256", "ecdsa-sha2-nistp256")
    live_rekey(ec_key, "curve25519-sha256", "ecdsa-sha2-nistp256")
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
    run_live_shell(ec_key, rsa_key)
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
        test_packet_in_pieces(v)
    end
    check(#SIGNATURES == 5, "the uncommon signature encodings")
    for _, v in ipairs(SIGNATURES) do
        test_recorded_signatures(v)
    end
    test_signature_algorithm_binding()
    test_what_is_offered()
    test_directions_negotiated_separately()
    test_hostile_lengths()
    test_hostile_padding()
    test_stream_stays_dead()
    test_hostile_strings()
    test_hostile_kexinit()
    test_hostile_banner()
    test_poll_packet_in_pieces()
    test_poll_ends_session()
    test_common_messages()
    test_global_request_reply()
    test_rekey_refused()
    test_messages_before_keys()
    test_session_replies()
    test_open_session()
    test_requests_before_reply()
    test_typing_after_shell()
    test_large_windows()
    test_window_stays_under_the_tcp_limit()
    test_packet_framing()
    test_mpint()
    test_disconnect_reason()
    test_manifest()
    test_keys()
    test_cursor_key_mode()
    test_close_unopened_channel()
end
