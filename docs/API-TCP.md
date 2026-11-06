---
title: "API TCP"
---

Raw non-blocking TCP/TLS client sockets. Connections are handled cross-core by the Core 1 network stack. A static pool supports up to 4 simultaneous connections.

**Requirements:** declare `"http"` in your `app.json` `requirements` for TCP too. It makes the OS connect to the saved network when your app launches and keep the connection up while it runs. Without it nothing refuses your calls, but WiFi may well be off, and a connect then fails. The simulator is always online, so a missing requirement shows only on the device.

## picocalc.tcp

Callbacks fire from the main loop — they are pumped by the firmware's debug hook. Call `picocalc.input.update()` or `picocalc.sys.sleep()` regularly in your main loop so they can run.

### Functions

#### `picocalc.tcp.new(host [, port] [, use_ssl])`
Open a new TCP connection to a host.

- **Parameters:**
  - `host` (string): Hostname or IP address
  - `port` (number, optional): Port number. Default 80, with or without TLS. To pass `use_ssl` without a port, give `nil`: `new(host, nil, true)` (`new(host, true)` raises).
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
- **Returns:** (number) Number of bytes queued (all of them), or -1 if nothing was queued

-1 is usually not a failure. Each `write` is one request to the network core, and at most 8 can wait, so a tight loop gets -1 once they are queued, with the connection fine and `conn:error()` still `nil`. You also get -1 while more than 16 KB you wrote is waiting to go out, because the server is not reading or a lost frame is being resent. Send data in one call where you can (a single large write is fine), and retry the rest while the socket is connected:

```lua
local function write_all(conn, data)
    while true do
        local n = conn:write(data)
        if n >= 0 then return true end
        if not conn:isConnected() then return false, conn:error() end
        picocalc.sys.sleep(5)   -- the queue drains within a few milliseconds
    end
end

local ok, err = write_all(conn, "GET / HTTP/1.0\r\n\r\n")
```

**A lost frame usually costs little.** If WiFi loses a frame, the device's TCP stack sends it again, at once if more data followed it, otherwise a second later, then 2, 4 and so on up to 60 seconds apart until it gets through, and the stream carries on with nothing lost and no error. HTTP and TLS run over the same stack. So the last write before a pause is the one that can take a second to arrive. The stack also sends at most 6000 bytes ahead of what the server has acknowledged, which limits an upload to 6000 bytes per round trip.

If the server stops answering altogether, the socket fails with `Mongoose error: keepalive` about 3 minutes after the last thing it heard. `setReadTimeout` finds out sooner, if your protocol expects replies. A connection request goes again after 1, 3 and 7 seconds, and the connect fails after 15 if nothing has answered.

---

#### `conn:read([max_len])`
Read available data from the connection.

- **Parameters:**
  - `max_len` (number, optional): Maximum number of bytes to read. Default 4096, so `conn:read()` can leave data behind: loop while `conn:available() > 0`.
- **Returns:** (string or nil) Data read, or `nil` if no data is available

Read as data arrives: the device has no TCP flow control, so a connection whose unread data passes 32 KB fails with `receive overflow (the app is not reading fast enough)` (`conn:error()`). Data already received stays readable after the peer closes, however much of it there is. A TLS server's close_notify ends the connection like a plain close (`isConnected()` false, no error).

```lua
local chunk = conn:read(1024)
```

---

#### `conn:close()`
Close the connection.

The object is unusable afterwards (I/O raises). Data already received stays readable until close, including after the peer closes.

What you already wrote is still sent, and then the server sees the connection close. That finishes in the background, for at most 10 seconds if the server is not responding, and the socket can be reused at once. Anything the server sends after your `close()` is dropped. If that last closing message itself is lost, which is not resent, the server finds out the next time it sends.

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
while conn:available() > 0 do
    local data = conn:read()
end
```

---

#### `conn:error()`
Get the last connection error.

- **Parameters:** None
- **Returns:** (string or nil) Error string, or `nil` if there is no error. It stays `nil` while the socket is connecting or connected.

---

#### `conn:isConnected()`
Check whether the connection is established.

- **Parameters:** None
- **Returns:** (boolean) `true` if connected

---

#### `conn:setConnectTimeout(seconds)`
Set the connection timeout.

Applies to the next connect; default 15 s (covers the TLS handshake). On a device, an address that never answers fails after 15 s even with a longer timeout, because the network stack stops trying then. A slow TLS handshake to a server that did answer gets the whole timeout.

- **Parameters:**
  - `seconds` (number): Timeout in seconds. Fractions are allowed. `0` turns it off.
- **Returns:** None

```lua
conn:setConnectTimeout(10)
```

---

#### `conn:setReadTimeout(seconds)`
Fail a connected socket that receives nothing for this long. Every byte that arrives starts it over.

Off by default; 0 disables it.

- **Parameters:**
  - `seconds` (number): Timeout in seconds. Fractions are allowed.
- **Returns:** None

---

#### `conn:setConnectCallback(fn)`
Set a callback fired when the connection is established. The callback receives the connection object. It does not fire when the connection fails: that is the [close callback](#connsetclosecallbackfn).

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
Set a callback fired when the connection ends: the peer closed it, or the connection failed (including a connect that never succeeded and a timeout; `c:error()` says why, and is `nil` after a clean close). It does not fire after your own `conn:close()`. The callback receives the connection object.

- **Parameters:**
  - `fn` (function): `function(conn) ... end`
- **Returns:** None

---

#### `conn:getEvents()`
Get pending connection events.

- **Parameters:** None
- **Returns:** (number) the pending events that have **no callback registered**, cleared by the call: `picocalc.tcp.CB_CONNECT` (1), `CB_READ` (2), `CB_CLOSED` (8), `CB_FAILED` (16). `CB_WRITE` (4) is reserved: it is never set.

---

#### `conn:waitConnected([timeout_seconds])`
Block until the connection is established. Your callbacks fire during the wait, the system menu can open, and an exit request ends it, as in `picocalc.sys.sleep`, so a long timeout is safe.

- **Parameters:**
  - `timeout_seconds` (number, optional): Maximum time to wait. Default 10. Fractions are allowed.
- **Returns:** (boolean) `true` if connected, `false` on timeout or failure

```lua
if conn:waitConnected(5) then
    conn:write("hello")
end
```

---

#### `conn:waitData([timeout_seconds])`
Block until data is available to read. Callbacks, the system menu and exit work during the wait, as for `waitConnected`.

- **Parameters:**
  - `timeout_seconds` (number, optional): Maximum time to wait. Default 10. Fractions are allowed.
- **Returns:** (boolean) `true` if data arrived, `false` on timeout or when the connection ended with nothing left to read

```lua
if conn:waitData(2) then
    local data = conn:read()
end
```
