-- ssh_connection.lua — SSH-2 connection layer (RFC 4254)
-- Manages channels, PTY requests, and data flow.

local wire = require("lib.ssh_wire")

local connection = {}
connection.__index = connection

local MSG = {
    UNIMPLEMENTED         = 3,
    CHANNEL_OPEN          = 90,
    CHANNEL_OPEN_CONFIRM  = 91,
    CHANNEL_OPEN_FAILURE  = 92,
    CHANNEL_WINDOW_ADJUST = 93,
    CHANNEL_DATA          = 94,
    CHANNEL_EXTENDED_DATA = 95,
    CHANNEL_EOF           = 96,
    CHANNEL_CLOSE         = 97,
    CHANNEL_REQUEST       = 98,
    CHANNEL_SUCCESS       = 99,
    CHANNEL_FAILURE       = 100,
}

local INITIAL_WINDOW = 64 * 1024    -- 64KB
local MAX_PACKET     = 32 * 1024    -- 32KB
local WINDOW_THRESHOLD = 32 * 1024  -- replenish at 32KB consumed
-- A window is a uint32, but a read past 2^31 is negative on the device's 32-bit
-- integers, and no keyboard needs that much: stop there.
local MAX_REMOTE_WINDOW = 0x7FFFFFFF

local function grow_window(window, n)
    if n < 0 or n > MAX_REMOTE_WINDOW - window then return MAX_REMOTE_WINDOW end
    return window + n
end

function connection.new(transport)
    local self = setmetatable({}, connection)
    self.transport = transport
    self.local_channel = 0
    self.remote_channel = nil
    self.remote_window = 0
    self.local_window = INITIAL_WINDOW
    self.window_consumed = 0
    self.closed = false
    self.eof_received = false
    self.data_buf = ""
    self.error = nil
    return self
end

-- Open a "session" channel
function connection:open_session()
    local payload = string.char(MSG.CHANNEL_OPEN)
                 .. wire.string("session")
                 .. wire.uint32(self.local_channel)    -- sender channel
                 .. wire.uint32(INITIAL_WINDOW)        -- initial window size
                 .. wire.uint32(MAX_PACKET)            -- max packet size
    self.transport:send_packet(payload)

    local reply = self.transport:recv_packet_skip(10000)
    if not reply then
        self.error = self.transport.error
        return false
    end

    local msg_type = string.byte(reply, 1)
    if msg_type == MSG.CHANNEL_OPEN_FAILURE then
        local reason_code = wire.read_uint32(reply, 6)
        local desc = wire.read_string(reply, 10) or "unknown"
        self.error = "channel open failed: " .. desc .. " (code=" .. (reason_code or 0) .. ")"
        return false
    end

    if msg_type ~= MSG.CHANNEL_OPEN_CONFIRM then
        self.error = "expected CHANNEL_OPEN_CONFIRM, got " .. msg_type
        return false
    end

    local pos = 2
    local recipient_channel, remote_channel, remote_window, remote_max_packet
    recipient_channel, pos = wire.read_uint32(reply, pos)  -- our channel
    remote_channel, pos = wire.read_uint32(reply, pos)
    remote_window, pos = wire.read_uint32(reply, pos)
    remote_max_packet, pos = wire.read_uint32(reply, pos)
    if not remote_max_packet or recipient_channel ~= self.local_channel then
        self.error = "malformed CHANNEL_OPEN_CONFIRM"
        return false
    end
    self.remote_channel = remote_channel
    self.remote_window = grow_window(0, remote_window)

    return true
end

-- Wait for the reply to a channel request. A window adjust or output can come first
-- (OpenSSH grants its window just before replying to "shell"): act on those as usual.
function connection:wait_for_reply(what)
    while true do
        local reply = self.transport:recv_packet_skip(10000)
        if not reply then
            self.error = self.transport.error
            return false
        end

        local msg_type = string.byte(reply, 1)
        if msg_type == MSG.CHANNEL_SUCCESS then
            return true
        elseif msg_type == MSG.CHANNEL_FAILURE then
            self.error = what .. " failed"
            return false
        end

        self:process_packet(reply)
        if self.closed then
            self.error = "channel closed before the reply to " .. what
            return false
        end
    end
end

-- Request a pseudo-terminal
function connection:request_pty(term_type, cols, rows)
    term_type = term_type or "xterm-256color"
    cols = cols or 53
    rows = rows or 26

    local modes = wire.string("")  -- empty terminal modes (encoded per RFC 4254)

    local payload = string.char(MSG.CHANNEL_REQUEST)
                 .. wire.uint32(self.remote_channel)
                 .. wire.string("pty-req")
                 .. wire.boolean(true)      -- want reply
                 .. wire.string(term_type)
                 .. wire.uint32(cols)        -- terminal width, chars
                 .. wire.uint32(rows)        -- terminal height, rows
                 .. wire.uint32(0)           -- terminal width, pixels
                 .. wire.uint32(0)           -- terminal height, pixels
                 .. modes
    self.transport:send_packet(payload)

    return self:wait_for_reply("pty-req")
end

-- Request shell
function connection:request_shell()
    local payload = string.char(MSG.CHANNEL_REQUEST)
                 .. wire.uint32(self.remote_channel)
                 .. wire.string("shell")
                 .. wire.boolean(true)  -- want reply
    self.transport:send_packet(payload)

    return self:wait_for_reply("shell request")
end

-- Send data to the channel
function connection:write(data)
    if self.closed then return false end

    while #data > 0 do
        -- Wait for window space
        if self.remote_window <= 0 then
            -- Try to read a window adjust
            self:poll(100)
            if self.remote_window <= 0 then
                return false  -- no window space
            end
        end

        local chunk_size = #data
        if chunk_size > self.remote_window then chunk_size = self.remote_window end
        if chunk_size > MAX_PACKET then chunk_size = MAX_PACKET end

        local payload = string.char(MSG.CHANNEL_DATA)
                     .. wire.uint32(self.remote_channel)
                     .. wire.string(data:sub(1, chunk_size))
        self.transport:send_packet(payload)

        self.remote_window = self.remote_window - chunk_size
        data = data:sub(chunk_size + 1)
    end

    return true
end

-- Process a single incoming packet (non-blocking)
function connection:process_packet(payload)
    if not payload then return false end

    local msg_type = string.byte(payload, 1)

    if msg_type == MSG.CHANNEL_DATA then
        local _, pos = wire.read_uint32(payload, 2)  -- recipient channel
        local data = wire.read_string(payload, pos)
        if data then
            self.data_buf = self.data_buf .. data
            self.window_consumed = self.window_consumed + #data
        end

    elseif msg_type == MSG.CHANNEL_EXTENDED_DATA then
        local _, pos = wire.read_uint32(payload, 2)  -- recipient channel
        local data_type
        data_type, pos = wire.read_uint32(payload, pos)
        local data = wire.read_string(payload, pos)
        if data then
            -- stderr (type 1) — mix into main data stream for terminal display
            self.data_buf = self.data_buf .. data
            self.window_consumed = self.window_consumed + #data
        end

    elseif msg_type == MSG.CHANNEL_WINDOW_ADJUST then
        local _, pos = wire.read_uint32(payload, 2)
        local bytes_to_add = wire.read_uint32(payload, pos)
        if bytes_to_add then
            self.remote_window = grow_window(self.remote_window, bytes_to_add)
        end

    elseif msg_type == MSG.CHANNEL_EOF then
        self.eof_received = true

    elseif msg_type == MSG.CHANNEL_CLOSE then
        self.closed = true
        -- Send close back if we haven't already
        local close_payload = string.char(MSG.CHANNEL_CLOSE)
                           .. wire.uint32(self.remote_channel)
        self.transport:send_packet(close_payload)

    elseif msg_type == MSG.CHANNEL_REQUEST then
        -- Server-initiated requests: exit-status and exit-signal want no reply. Any
        -- that does (keepalive@openssh.com) is not one this client supports: refuse it.
        local _, pos = wire.read_uint32(payload, 2)
        local req_type
        req_type, pos = wire.read_string(payload, pos)
        local want_reply = wire.read_boolean(payload, pos)
        if want_reply then
            self.transport:send_packet(string.char(MSG.CHANNEL_FAILURE)
                                       .. wire.uint32(self.remote_channel))
        end

    elseif msg_type == MSG.CHANNEL_OPEN then
        -- RFC 4254 section 5.1: 1 = SSH_OPEN_ADMINISTRATIVELY_PROHIBITED
        local _, pos = wire.read_string(payload, 2)  -- channel type
        local sender_channel = wire.read_uint32(payload, pos)
        if sender_channel then
            self.transport:send_packet(string.char(MSG.CHANNEL_OPEN_FAILURE)
                                       .. wire.uint32(sender_channel) .. wire.uint32(1)
                                       .. wire.string("not supported") .. wire.string(""))
        end

    elseif msg_type == MSG.CHANNEL_SUCCESS or msg_type == MSG.CHANNEL_FAILURE then
        -- The reply to a request nothing is still waiting on

    else
        -- RFC 4253 section 11.4: answer anything unknown, with its sequence number
        self.transport:send_packet(string.char(MSG.UNIMPLEMENTED)
                                   .. wire.uint32(self.transport.recv_seq - 1))
    end

    -- Replenish window if we've consumed enough
    if self.window_consumed >= WINDOW_THRESHOLD then
        local adjust = string.char(MSG.CHANNEL_WINDOW_ADJUST)
                    .. wire.uint32(self.remote_channel)
                    .. wire.uint32(self.window_consumed)
        self.transport:send_packet(adjust)
        self.local_window = self.local_window + self.window_consumed
        self.window_consumed = 0
    end

    return true
end

-- Poll for incoming data (non-blocking, returns quickly if nothing available).
-- If the connection has gone, self.closed is set and self.error says why.
function connection:poll(timeout_ms)
    timeout_ms = timeout_ms or 0
    local transport = self.transport

    local start = picocalc.sys.getTimeMs()
    while true do
        local alive = transport:fill_buf()

        -- Whole packets only: one still arriving stays buffered for the next poll.
        local payload = transport:take_message()
        while payload do
            self:process_packet(payload)
            payload = transport:take_message()
        end

        if not alive and not self.closed then transport:connection_closed() end
        if transport.dead and not self.closed then
            self.closed = true
            self.error = transport.error
        end

        if #self.data_buf > 0 then return end  -- have data to deliver
        if self.closed then return end

        if timeout_ms <= 0 then return end
        if picocalc.sys.getTimeMs() - start > timeout_ms then return end

        picocalc.sys.sleep(5)
    end
end

-- Read buffered data (non-blocking)
function connection:read()
    if #self.data_buf == 0 then return nil end
    local data = self.data_buf
    self.data_buf = ""
    return data
end

-- Send disconnect and close
function connection:close()
    if not self.closed then
        local payload = string.char(MSG.CHANNEL_CLOSE)
                     .. wire.uint32(self.remote_channel)
        self.transport:send_packet(payload)
        self.closed = true
    end
end

return connection
