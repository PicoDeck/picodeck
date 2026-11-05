-- ssh_transport.lua — SSH-2 transport layer
-- Handles version exchange, key exchange, encryption, packet framing.

local wire = require("lib.ssh_wire")
local crypto = picocalc.crypto
local tcp = picocalc.tcp

local transport = {}
transport.__index = transport

-- SSH message types
local MSG = {
    DISCONNECT           = 1,
    IGNORE               = 2,
    UNIMPLEMENTED        = 3,
    DEBUG                = 4,
    SERVICE_REQUEST      = 5,
    SERVICE_ACCEPT       = 6,
    KEXINIT              = 20,
    NEWKEYS              = 21,
    KEX_ECDH_INIT        = 30,
    KEX_ECDH_REPLY       = 31,
    USERAUTH_REQUEST     = 50,
    USERAUTH_FAILURE     = 51,
    USERAUTH_SUCCESS     = 52,
    USERAUTH_BANNER      = 53,
    CHANNEL_OPEN         = 90,
    CHANNEL_OPEN_CONFIRM = 91,
    CHANNEL_OPEN_FAILURE = 92,
    CHANNEL_WINDOW_ADJUST= 93,
    CHANNEL_DATA         = 94,
    CHANNEL_EOF          = 96,
    CHANNEL_CLOSE        = 97,
    CHANNEL_REQUEST      = 98,
    CHANNEL_SUCCESS      = 99,
    CHANNEL_FAILURE      = 100,
}
transport.MSG = MSG

-- Algorithm preferences
local CLIENT_KEX       = {"curve25519-sha256", "ecdh-sha2-nistp256"}
-- Only what picocalc.crypto can verify: never "ssh-rsa", whose signatures are SHA-1.
local CLIENT_HOSTKEY   = {"rsa-sha2-256", "ecdsa-sha2-nistp256"}
local HOSTKEY_KEY_TYPE = {
    ["rsa-sha2-256"] = "ssh-rsa",  -- RFC 8332: the key blob keeps its old name
    ["ecdsa-sha2-nistp256"] = "ecdsa-sha2-nistp256",
}
local CLIENT_CIPHER    = {"aes128-ctr", "aes256-ctr"}
local CLIENT_MAC       = {"hmac-sha2-256", "hmac-sha1"}
local CLIENT_COMPRESS  = {"none"}

-- ── Constructor ─────────────────────────────────────────────────────────────

function transport.new(conn)
    local self = setmetatable({}, transport)
    self.conn = conn               -- TCP connection userdata
    self.recv_buf = ""             -- accumulated receive buffer
    self.client_version = "SSH-2.0-PicoDeck_1.0"
    self.server_version = nil
    self.session_id = nil

    -- Sequence numbers
    self.send_seq = 0
    self.recv_seq = 0

    -- Encryption state (nil = plaintext)
    self.send_cipher = nil
    self.recv_cipher = nil
    self.send_mac_key = nil
    self.recv_mac_key = nil
    self.send_mac_algo = nil
    self.recv_mac_algo = nil
    self.block_size = 8            -- minimum (plaintext)

    -- Key exchange state
    self.kex_algo = nil
    self.hostkey_algo = nil
    self.cipher_algo = nil
    self.mac_algo = nil
    self.client_kexinit_payload = nil
    self.server_kexinit_payload = nil

    self.error = nil
    return self
end

-- ── Low-level I/O ───────────────────────────────────────────────────────────

-- Read available data from TCP into recv_buf.
-- Returns false if the connection is dead.
function transport:fill_buf()
    if not self.conn:isConnected() then
        return false
    end
    local data = self.conn:read(8192)
    if data then
        self.recv_buf = self.recv_buf .. data
    end
    return true
end

-- Read exactly n bytes from recv_buf, blocking with timeout
function transport:read_bytes(n, timeout_ms)
    timeout_ms = timeout_ms or 30000
    local start = picocalc.sys.getTimeMs()
    while #self.recv_buf < n do
        if not self:fill_buf() then
            local err = self.conn:error()
            self.error = "connection closed" .. (err and (": " .. err) or "")
            return nil
        end
        if #self.recv_buf >= n then break end
        if picocalc.sys.getTimeMs() - start > timeout_ms then
            self.error = "timeout reading " .. n .. " bytes"
            return nil
        end
        picocalc.sys.sleep(5)
    end
    local result = self.recv_buf:sub(1, n)
    self.recv_buf = self.recv_buf:sub(n + 1)
    return result
end

-- Read a line ending with \r\n (for version exchange)
function transport:read_line(timeout_ms)
    timeout_ms = timeout_ms or 10000
    local start = picocalc.sys.getTimeMs()
    while true do
        local i = self.recv_buf:find("\r\n", 1, true)
        if i then
            local line = self.recv_buf:sub(1, i - 1)
            self.recv_buf = self.recv_buf:sub(i + 2)
            return line
        end
        if not self:fill_buf() then
            local err = self.conn:error()
            self.error = "connection closed" .. (err and (": " .. err) or "")
            return nil
        end
        if picocalc.sys.getTimeMs() - start > timeout_ms then
            self.error = "timeout reading line"
            return nil
        end
        picocalc.sys.sleep(5)
    end
end

function transport:send_raw(data)
    local n = self.conn:write(data)
    if n < 0 then
        local err = self.conn:error()
        self.error = "write failed" .. (err and (": " .. err) or "")
        return false
    end
    return true
end

-- ── Packet send/receive ─────────────────────────────────────────────────────

function transport:send_packet(payload)
    local block_size = self.send_cipher and 16 or 8
    local packet = wire.build_packet(payload, block_size)

    local ok
    if self.send_cipher then
        -- Encrypt: the entire packet (length + padding_length + payload + padding)
        local encrypted = self.send_cipher:update(packet)

        -- MAC: HMAC over (sequence_number + unencrypted_packet)
        local mac_input = string.pack(">I4", self.send_seq) .. packet
        local mac
        if self.send_mac_algo == "hmac-sha2-256" then
            mac = crypto.hmacSHA256(self.send_mac_key, mac_input)
        else
            mac = crypto.hmacSHA1(self.send_mac_key, mac_input)
        end

        ok = self:send_raw(encrypted .. mac)
    else
        ok = self:send_raw(packet)
    end

    if not ok then return false end
    self.send_seq = self.send_seq + 1
    return true
end

function transport:recv_packet(timeout_ms)
    timeout_ms = timeout_ms or 30000
    local mac_len = 0
    if self.recv_mac_algo == "hmac-sha2-256" then mac_len = 32
    elseif self.recv_mac_algo == "hmac-sha1" then mac_len = 20
    end

    if self.recv_cipher then
        -- Read first block to get packet_length
        local block_size = 16
        local first_block = self:read_bytes(block_size, timeout_ms)
        if not first_block then return nil end

        local decrypted_first = self.recv_cipher:update(first_block)
        local packet_length = string.unpack(">I4", decrypted_first)

        -- Read remaining encrypted data
        local remaining = packet_length - (block_size - 4)
        local rest = ""
        if remaining > 0 then
            rest = self:read_bytes(remaining, timeout_ms)
            if not rest then return nil end
            rest = self.recv_cipher:update(rest)
        end

        local full_packet = decrypted_first .. rest

        -- Read and verify MAC
        if mac_len > 0 then
            local mac = self:read_bytes(mac_len, timeout_ms)
            if not mac then return nil end

            local mac_input = string.pack(">I4", self.recv_seq) .. full_packet
            local expected_mac
            if self.recv_mac_algo == "hmac-sha2-256" then
                expected_mac = crypto.hmacSHA256(self.recv_mac_key, mac_input)
            else
                expected_mac = crypto.hmacSHA1(self.recv_mac_key, mac_input)
            end

            if mac ~= expected_mac then
                self.error = "MAC verification failed"
                return nil
            end
        end

        self.recv_seq = self.recv_seq + 1

        -- Extract payload
        local padding_length = string.byte(full_packet, 5)
        local payload_length = packet_length - padding_length - 1
        return full_packet:sub(6, 5 + payload_length)
    else
        -- Plaintext mode
        local header = self:read_bytes(4, timeout_ms)
        if not header then return nil end

        local packet_length = string.unpack(">I4", header)
        if packet_length > 35000 then
            self.error = "packet too large: " .. packet_length
            return nil
        end

        local body = self:read_bytes(packet_length, timeout_ms)
        if not body then return nil end

        self.recv_seq = self.recv_seq + 1

        local padding_length = string.byte(body, 1)
        local payload_length = packet_length - padding_length - 1
        return body:sub(2, 1 + payload_length)
    end
end

-- Receive a packet, skipping IGNORE and DEBUG messages
function transport:recv_packet_skip(timeout_ms)
    while true do
        local payload = self:recv_packet(timeout_ms)
        if not payload then return nil end
        local msg_type = string.byte(payload, 1)
        if msg_type == MSG.IGNORE or msg_type == MSG.DEBUG then
            -- skip
        elseif msg_type == MSG.DISCONNECT then
            local _, reason, desc = wire.read_uint32(payload, 2)
            desc = wire.read_string(payload, desc) or "unknown"
            self.error = "server disconnected: " .. desc
            return nil
        else
            return payload
        end
    end
end

-- ── Version exchange ────────────────────────────────────────────────────────

function transport:exchange_versions()
    if not self:send_raw(self.client_version .. "\r\n") then return false end

    -- Read lines until we find one starting with "SSH-2.0-"
    for _ = 1, 20 do
        local line = self:read_line(10000)
        if not line then
            self.error = "timeout waiting for server version"
            return false
        end
        if line:sub(1, 4) == "SSH-" then
            if line:sub(1, 8) ~= "SSH-2.0-" then
                self.error = "unsupported SSH version: " .. line
                return false
            end
            self.server_version = line
            return true
        end
        -- Pre-version banner lines, ignore
    end
    self.error = "no SSH version string received"
    return false
end

-- ── Key exchange ────────────────────────────────────────────────────────────

function transport:build_kexinit()
    local cookie = crypto.randomBytes(16)
    local payload = string.char(MSG.KEXINIT) .. cookie
    payload = payload .. wire.namelist(CLIENT_KEX)
    payload = payload .. wire.namelist(CLIENT_HOSTKEY)
    payload = payload .. wire.namelist(CLIENT_CIPHER)     -- c2s
    payload = payload .. wire.namelist(CLIENT_CIPHER)     -- s2c
    payload = payload .. wire.namelist(CLIENT_MAC)        -- c2s
    payload = payload .. wire.namelist(CLIENT_MAC)        -- s2c
    payload = payload .. wire.namelist(CLIENT_COMPRESS)   -- c2s
    payload = payload .. wire.namelist(CLIENT_COMPRESS)   -- s2c
    payload = payload .. wire.namelist({})                -- languages c2s
    payload = payload .. wire.namelist({})                -- languages s2c
    payload = payload .. wire.boolean(false)              -- first_kex_packet_follows
    payload = payload .. wire.uint32(0)                   -- reserved
    return payload
end

function transport:parse_kexinit(payload)
    local pos = 2 + 16  -- skip msg_type + cookie
    local server = {}

    server.kex, pos = wire.read_namelist(payload, pos)
    server.hostkey, pos = wire.read_namelist(payload, pos)
    server.cipher_c2s, pos = wire.read_namelist(payload, pos)
    server.cipher_s2c, pos = wire.read_namelist(payload, pos)
    server.mac_c2s, pos = wire.read_namelist(payload, pos)
    server.mac_s2c, pos = wire.read_namelist(payload, pos)
    server.compress_c2s, pos = wire.read_namelist(payload, pos)
    server.compress_s2c, pos = wire.read_namelist(payload, pos)

    return server
end

function transport:negotiate_algorithms(server)
    self.kex_algo = wire.negotiate(CLIENT_KEX, server.kex)
    if not self.kex_algo then
        self.error = "no common kex algorithm"
        return false
    end

    self.hostkey_algo = wire.negotiate(CLIENT_HOSTKEY, server.hostkey)
    if not self.hostkey_algo then
        self.error = "no common hostkey algorithm"
        return false
    end

    self.cipher_algo = wire.negotiate(CLIENT_CIPHER, server.cipher_c2s)
    if not self.cipher_algo then
        self.error = "no common cipher algorithm"
        return false
    end

    self.mac_algo = wire.negotiate(CLIENT_MAC, server.mac_c2s)
    if not self.mac_algo then
        self.error = "no common MAC algorithm"
        return false
    end

    return true
end

-- Compute exchange hash H per RFC 4253
-- For curve25519-sha256 and ecdh-sha2-nistp256, the hash input is:
--   string V_C || string V_S || string I_C || string I_S ||
--   string K_S || string e || string f || mpint K
function transport:compute_exchange_hash(K_S, e, f, K)
    local data = wire.string(self.client_version)
              .. wire.string(self.server_version)
              .. wire.string(self.client_kexinit_payload)
              .. wire.string(self.server_kexinit_payload)
              .. wire.string(K_S)
              .. wire.string(e)
              .. wire.string(f)
              .. K  -- K is already mpint-encoded
    return crypto.sha256(data)
end

-- Verify server host key signature
function transport:verify_host_key(K_S, H, sig_blob)
    -- Parse outer signature blob: string(algo) + string(sig_data)
    local algo, pos = wire.read_string(sig_blob, 1)
    local sig_data = wire.read_string(sig_blob, pos)

    if not algo or not sig_data then
        self.error = "malformed signature blob"
        return false
    end

    -- The server must sign with the algorithm the exchange agreed on, using a key
    -- of that algorithm's type. The verifiers do not read the key's type field.
    if algo ~= self.hostkey_algo then
        self.error = "unexpected signature algorithm: " .. algo
        return false
    end
    local key_type = wire.read_string(K_S, 1)
    if key_type ~= HOSTKEY_KEY_TYPE[algo] then
        self.error = "unexpected host key type: " .. tostring(key_type)
        return false
    end

    -- A host signs H with the scheme's usual hash, so the digest under the
    -- signature is SHA-256(H), not H (RFC 5656 section 3.1.2, RFC 8332 section 3).
    local digest = crypto.sha256(H)
    if algo == "rsa-sha2-256" then
        return crypto.rsaVerify(K_S, sig_data, digest)
    end
    return crypto.ecdsaP256Verify(K_S, sig_data, digest)
end

-- Perform ECDH key exchange (curve25519-sha256 or ecdh-sha2-nistp256)
function transport:do_kex()
    -- Generate ephemeral key pair
    local ecdh
    if self.kex_algo == "curve25519-sha256" then
        ecdh = crypto.ecdh_x25519_new()
    else
        ecdh = crypto.ecdh_p256_new()
    end

    local e = ecdh:getPublicKey()  -- client ephemeral public key

    -- Send KEX_ECDH_INIT
    local init_payload = string.char(MSG.KEX_ECDH_INIT) .. wire.string(e)
    if not self:send_packet(init_payload) then
        ecdh:free()
        return false
    end

    -- Receive KEX_ECDH_REPLY
    local reply = self:recv_packet_skip(30000)
    if not reply then
        ecdh:free()
        return false
    end

    if string.byte(reply, 1) ~= MSG.KEX_ECDH_REPLY then
        ecdh:free()
        self.error = "expected KEX_ECDH_REPLY, got " .. string.byte(reply, 1)
        return false
    end

    local pos = 2
    local K_S  -- server host key blob
    K_S, pos = wire.read_string(reply, pos)
    local f    -- server ephemeral public key
    f, pos = wire.read_string(reply, pos)
    local sig_blob  -- host key signature
    sig_blob, pos = wire.read_string(reply, pos)

    if not K_S or not f or not sig_blob then
        ecdh:free()
        self.error = "malformed KEX_ECDH_REPLY"
        return false
    end

    -- Compute shared secret
    local shared, ecdh_err = ecdh:computeShared(f)
    ecdh:free()
    if not shared then
        self.error = ecdh_err or "ECDH shared secret failed"
        return false
    end

    local K_mpint = wire.mpint(shared)

    -- Compute exchange hash H
    local H = self:compute_exchange_hash(K_S, e, f, K_mpint)

    -- Verify host key signature
    if not self:verify_host_key(K_S, H, sig_blob) then
        if not self.error then self.error = "host key verification failed" end
        return false
    end

    -- First exchange: session_id = H
    if not self.session_id then
        self.session_id = H
    end

    -- Store for key derivation
    self._K_mpint = K_mpint
    self._H = H
    self._K_S = K_S

    return true
end

function transport:derive_keys()
    local K = self._K_mpint
    local H = self._H
    local sid = self.session_id

    local cipher_key_len = (self.cipher_algo == "aes256-ctr") and 32 or 16
    local iv_len = 16
    local mac_key_len = (self.mac_algo == "hmac-sha2-256") and 32 or 20

    -- RFC 4253 §7.2: key derivation
    local iv_c2s    = crypto.deriveKey(K, H, sid, "A", iv_len)
    local iv_s2c    = crypto.deriveKey(K, H, sid, "B", iv_len)
    local key_c2s   = crypto.deriveKey(K, H, sid, "C", cipher_key_len)
    local key_s2c   = crypto.deriveKey(K, H, sid, "D", cipher_key_len)
    local mackey_c2s = crypto.deriveKey(K, H, sid, "E", mac_key_len)
    local mackey_s2c = crypto.deriveKey(K, H, sid, "F", mac_key_len)

    return {
        iv_c2s = iv_c2s, iv_s2c = iv_s2c,
        key_c2s = key_c2s, key_s2c = key_s2c,
        mackey_c2s = mackey_c2s, mackey_s2c = mackey_s2c,
    }
end

function transport:activate_keys(keys)
    -- Send NEWKEYS
    if not self:send_packet(string.char(MSG.NEWKEYS)) then return false end

    -- Receive NEWKEYS
    local reply = self:recv_packet_skip(10000)
    if not reply or string.byte(reply, 1) ~= MSG.NEWKEYS then
        self.error = "expected NEWKEYS"
        return false
    end

    -- Activate send cipher
    self.send_cipher = crypto.aes_ctr_new(keys.key_c2s, keys.iv_c2s)
    self.send_mac_key = keys.mackey_c2s
    self.send_mac_algo = self.mac_algo

    -- Activate recv cipher
    self.recv_cipher = crypto.aes_ctr_new(keys.key_s2c, keys.iv_s2c)
    self.recv_mac_key = keys.mackey_s2c
    self.recv_mac_algo = self.mac_algo

    return true
end

-- ── Full handshake ──────────────────────────────────────────────────────────

function transport:handshake()
    -- Version exchange
    if not self:exchange_versions() then return false end

    -- Send our KEXINIT
    self.client_kexinit_payload = self:build_kexinit()
    if not self:send_packet(self.client_kexinit_payload) then return false end

    -- Receive server KEXINIT
    local server_kexinit = self:recv_packet_skip(10000)
    if not server_kexinit then return false end

    if string.byte(server_kexinit, 1) ~= MSG.KEXINIT then
        self.error = "expected KEXINIT, got " .. string.byte(server_kexinit, 1)
        return false
    end

    self.server_kexinit_payload = server_kexinit

    -- Negotiate algorithms
    local server = self:parse_kexinit(server_kexinit)
    if not self:negotiate_algorithms(server) then return false end

    -- ECDH key exchange
    if not self:do_kex() then return false end

    -- Derive and activate keys
    local keys = self:derive_keys()
    if not self:activate_keys(keys) then return false end

    return true
end

-- ── Cleanup ─────────────────────────────────────────────────────────────────

function transport:close()
    if self.send_cipher then
        self.send_cipher:free()
        self.send_cipher = nil
    end
    if self.recv_cipher then
        self.recv_cipher:free()
        self.recv_cipher = nil
    end
end

-- Get host key fingerprint (SHA-256 hex)
function transport:get_host_key_fingerprint()
    if not self._K_S then return nil end
    local hash = crypto.sha256(self._K_S)
    local hex = {}
    for i = 1, #hash do
        hex[i] = string.format("%02x", string.byte(hash, i))
    end
    return table.concat(hex, ":")
end

-- Get host key algorithm name parsed from the key blob
function transport:get_host_key_type()
    if not self._K_S then return nil end
    local algo = wire.read_string(self._K_S, 1)
    return algo
end

return transport
