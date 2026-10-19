---
title: "API — TCP Sockets"
---

Raw non-blocking TCP/TLS client sockets. Connections are handled cross-core by the Core 1 network stack. A static pool supports up to 4 simultaneous connections.

## picocalc.tcp

Callbacks fire from the main loop — they are pumped by the firmware's debug hook. Call `picocalc.input.update()` or `picocalc.sys.sleep()` regularly in your main loop so they can run.

### Functions

#### `picocalc.tcp.new(host [, port] [, use_ssl])`
Open a new TCP connection to a host.

- **Parameters:**
  - `host` (string): Hostname or IP address
  - `port` (number, optional): Port number. Default 80.
  - `use_ssl` (boolean, optional): `true` for a TLS connection. Default `false`. TLS verifies the server certificate (OS root bundle + host name); a TLS connect before SNTP has set the clock fails with "clock not set". The socket counts as connected only after the TLS handshake.
- **Returns:** a connection object, or `nil, err` when the 4-socket pool is full. Nothing is sent until `conn:connect()`.

```lua
local conn = picocalc.tcp.new("example.com", 443, true)
```

---

### Connection Methods

Objects returned by `picocalc.tcp.new()`.

#### `conn:connect()`
Start connecting (non-blocking).

- **Returns:** `true`, or `false, err` (WiFi not available, already connecting or connected)

Completion: `isConnected()`, `waitConnected()`, the connect callback or
`picocalc.tcp.CB_CONNECT` in `getEvents()`.

---

#### `conn:setInsecure(flag)`
Before `connect()`: TLS without certificate verification or the clock check,
for self-signed servers: development servers, or servers whose certificate the
app pins itself with `peerFingerprint()`. Default `false`.

---

#### `conn:peerFingerprint()`
The SHA-256 of the TLS server's certificate, for apps that pin certificates
themselves (trust on first use, as Gemini clients do). Works with and without
`setInsecure`.

- **Returns:** (string or nil) 64 lowercase hex digits once connected, still
  there after the server closes or the connection fails, until the next
  `connect()`; `nil` for a plain socket, before the handshake completes,
  after `close()`, or when the server sent no certificate.

```lua
local conn = picocalc.tcp.new("example.org", 1965, true)
conn:setInsecure(true)      -- self-signed: compare the fingerprint instead
conn:connect()
if conn:waitConnected(20) and conn:peerFingerprint() ~= pinned then
    conn:close()            -- not the certificate seen before
end
```

---

#### `conn:write(data)`
Write data to the connection.

- **Parameters:**
  - `data` (string): Data to send
- **Returns:** (number) Number of bytes written, or -1 on error

```lua
conn:write("GET / HTTP/1.0\r\n\r\n")
```

---

#### `conn:read([max_len])`
Read available data from the connection.

- **Parameters:**
  - `max_len` (number, optional): Maximum number of bytes to read
- **Returns:** (string or nil) Data read, or `nil` if no data is available

Read as data arrives: the device has no TCP flow control, so a connection whose unread data passes 32 KB fails with `receive overflow (the app is not reading fast enough)` (`conn:error()`). Data already received stays readable after the peer closes, however much of it there is. A TLS server's close_notify ends the connection like a plain close (`isConnected()` false, no error).

```lua
local chunk = conn:read(1024)
```

---

#### `conn:close()`
Close the connection.

The object is unusable afterwards (I/O raises). Data already received stays readable until close, including after the peer closes.

- **Parameters:** None
- **Returns:** None

```lua
conn:close()
```

---

#### `conn:available()`
Get the number of bytes available to read.

- **Parameters:** None
- **Returns:** (number) Bytes available

```lua
if conn:available() > 0 then
    local data = conn:read()
end
```

---

#### `conn:error()`
Get the last connection error.

- **Parameters:** None
- **Returns:** (string or nil) Error string, or `nil` if there is no error

---

#### `conn:isConnected()`
Check whether the connection is established.

- **Parameters:** None
- **Returns:** (boolean) `true` if connected

---

#### `conn:setConnectTimeout(seconds)`
Set the connection timeout.

Applies to the next connect; default 15 s (covers the TLS handshake).

- **Parameters:**
  - `seconds` (number): Timeout in seconds
- **Returns:** None

```lua
conn:setConnectTimeout(10)
```

---

#### `conn:setReadTimeout(seconds)`
Set the read timeout.

Off by default; 0 disables it.

- **Parameters:**
  - `seconds` (number): Timeout in seconds
- **Returns:** None

---

#### `conn:setConnectCallback(fn)`
Set a callback fired when the connection is established (or fails). The callback receives the connection object.

Callbacks receive the socket and are never nested.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

```lua
conn:setConnectCallback(function(c)
    picocalc.sys.log("connected")
end)
```

---

#### `conn:setReadCallback(fn)`
Set a callback fired when data arrives. The callback receives the connection object.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

```lua
conn:setReadCallback(function(c)
    local data = c:read()
    if data then picocalc.sys.log("got " .. #data .. " bytes") end
end)
```

---

#### `conn:setCloseCallback(fn)`
Set a callback fired when the connection closes. The callback receives the connection object.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

---

#### `conn:getEvents()`
Get pending connection events.

- **Parameters:** None
- **Returns:** (number) the pending events that have **no callback registered**, cleared by the call: `picocalc.tcp.CB_CONNECT` (1), `CB_READ` (2), `CB_WRITE` (4), `CB_CLOSED` (8), `CB_FAILED` (16).

---

#### `conn:waitConnected([timeout_seconds])`
Block until the connection is established.

- **Parameters:**
  - `timeout_seconds` (number, optional): Maximum time to wait
- **Returns:** (boolean) `true` if connected, `false` on timeout or failure

```lua
if conn:waitConnected(5) then
    conn:write("hello")
end
```

---

#### `conn:waitData([timeout_seconds])`
Block until data is available to read.

- **Parameters:**
  - `timeout_seconds` (number, optional): Maximum time to wait
- **Returns:** (boolean) `true` if data arrived, `false` on timeout

```lua
if conn:waitData(2) then
    local data = conn:read()
end
```
