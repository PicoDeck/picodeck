-- ssh_wire.lua — SSH binary wire format helpers
-- All SSH integers are big-endian. Strings are uint32-length-prefixed.

local wire = {}

local pack = string.pack
local unpack = string.unpack
local char = string.char
local byte = string.byte
local sub = string.sub
local concat = table.concat

-- ── Encoding ────────────────────────────────────────────────────────────────

-- Encode a uint32 (4 bytes, big-endian)
function wire.uint32(n)
    return pack(">I4", n)
end

-- Encode an SSH string (uint32 length + raw bytes)
function wire.string(s)
    return pack(">I4", #s) .. s
end

-- Encode a boolean (1 byte)
function wire.boolean(b)
    return char(b and 1 or 0)
end

-- Encode a byte
function wire.byte(b)
    return char(b)
end

-- Encode a name-list (comma-separated strings)
function wire.namelist(names)
    if type(names) == "table" then
        names = concat(names, ",")
    end
    return wire.string(names)
end

-- Encode an mpint (multi-precision integer from big-endian binary string)
-- Input: raw big-endian byte string representing the integer
-- Output: SSH mpint encoding
function wire.mpint(bytes)
    -- Strip leading zero bytes
    local i = 1
    while i < #bytes and byte(bytes, i) == 0 do
        i = i + 1
    end
    bytes = sub(bytes, i)

    if #bytes == 0 then
        return pack(">I4", 0)
    end

    -- If high bit is set, prepend a zero byte (positive mpint)
    if byte(bytes, 1) >= 128 then
        bytes = "\0" .. bytes
    end

    return pack(">I4", #bytes) .. bytes
end

-- Encode an mpint from a raw shared secret (may need zero-padding for length)
function wire.mpint_from_secret(bytes)
    return wire.mpint(bytes)
end

-- ── Decoding ────────────────────────────────────────────────────────────────

-- Read a uint32 at position pos, return (value, next_pos)
function wire.read_uint32(buf, pos)
    pos = pos or 1
    if pos + 3 > #buf then return nil, pos end
    local val, npos = unpack(">I4", buf, pos)
    return val, npos
end

-- Read an SSH string at position pos, return (string, next_pos)
function wire.read_string(buf, pos)
    pos = pos or 1
    if pos + 3 > #buf then return nil, pos end
    local len, npos = unpack(">I4", buf, pos)
    -- On the device's 32-bit integers a length past 2^31 is negative, and adding
    -- to one near it wraps: compare with the bytes left, and never add first.
    if len < 0 or len > #buf - npos + 1 then return nil, pos end
    return sub(buf, npos, npos + len - 1), npos + len
end

-- Read a byte at position pos
function wire.read_byte(buf, pos)
    pos = pos or 1
    if pos > #buf then return nil, pos end
    return byte(buf, pos), pos + 1
end

-- Read a boolean at position pos
function wire.read_boolean(buf, pos)
    local b, npos = wire.read_byte(buf, pos)
    if b == nil then return nil, pos end
    return b ~= 0, npos
end

-- Read an mpint at position pos, return (raw big-endian bytes without leading zeros, next_pos)
function wire.read_mpint(buf, pos)
    local s, npos = wire.read_string(buf, pos)
    if not s then return nil, pos end
    -- Strip leading zero bytes (sign padding)
    local i = 1
    while i < #s and byte(s, i) == 0 do
        i = i + 1
    end
    return sub(s, i), npos
end

-- Read a name-list, return (table of strings, next_pos)
function wire.read_namelist(buf, pos)
    local s, npos = wire.read_string(buf, pos)
    if not s then return nil, pos end
    if #s == 0 then return {}, npos end
    local names = {}
    for name in s:gmatch("[^,]+") do
        names[#names + 1] = name
    end
    return names, npos
end

-- ── Packet framing ──────────────────────────────────────────────────────────

-- Build an unencrypted SSH packet from payload
-- Returns: full packet (uint32 packet_length + byte padding_length + payload + padding)
function wire.build_packet(payload, block_size)
    block_size = block_size or 8
    if block_size < 8 then block_size = 8 end

    -- packet_length = 1 (padding_length) + payload_length + padding_length
    -- RFC 4253 section 6: the whole packet (4 length + 1 pad_len + payload + padding)
    -- is a multiple of block_size, with at least 4 bytes of padding.
    local padding = block_size - (5 + #payload) % block_size
    if padding < 4 then padding = padding + block_size end

    local packet_length = 1 + #payload + padding
    local pad = string.rep("\0", padding)

    -- Generate random padding if crypto is available
    if picocalc and picocalc.crypto then
        pad = picocalc.crypto.randomBytes(padding)
    end

    return pack(">I4", packet_length) .. char(padding) .. payload .. pad
end

-- Parse a received packet, return (payload, next_pos)
-- buf should start at the beginning of the packet
function wire.parse_packet(buf, pos)
    pos = pos or 1
    if pos + 3 > #buf then return nil, nil, pos end

    local packet_length, p2 = unpack(">I4", buf, pos)
    if p2 + packet_length - 1 > #buf then return nil, nil, pos end  -- incomplete

    local padding_length = byte(buf, p2)
    local payload_length = packet_length - padding_length - 1
    local payload = sub(buf, p2 + 1, p2 + payload_length)
    local next_pos = p2 + packet_length

    return payload, packet_length + 4, next_pos
end

-- ── Algorithm negotiation ───────────────────────────────────────────────────

-- Find first common algorithm between client and server lists
function wire.negotiate(client_list, server_list)
    for _, c in ipairs(client_list) do
        for _, s in ipairs(server_list) do
            if c == s then return c end
        end
    end
    return nil
end

return wire
