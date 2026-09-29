---
title: "API Network and WiFi"
---

WiFi connectivity and HTTP client.

## picocalc.wifi

WiFi connectivity functions (Pico 2W only).

### Functions

#### `picocalc.wifi.isAvailable()`
Checks if WiFi hardware is present.

- **Parameters:** None
- **Returns:** (boolean) `true` if WiFi is available (Pico 2W), `false` otherwise

```lua
if picocalc.wifi.isAvailable() then
    picocalc.wifi.connect("MySSID", "password")
end
```

---

#### `picocalc.wifi.connect(ssid [, password])`
Connects to a WiFi network (non-blocking).

- **Parameters:**
  - `ssid` (string): Network SSID
  - `password` (string, optional): Network password (omit for open networks)
- **Returns:** None

```lua
picocalc.wifi.connect("MyWiFi", "password123")
```

---

#### `picocalc.wifi.disconnect()`
Disconnects from the current WiFi network.

- **Parameters:** None
- **Returns:** None

---

#### `picocalc.wifi.getStatus()`
Returns the current WiFi connection status.

- **Parameters:** None
- **Returns:** (number) Status code (see constants below)

```lua
if picocalc.wifi.getStatus() == picocalc.wifi.STATUS_CONNECTED then
    print("Connected! IP: " .. picocalc.wifi.getIP())
end
```

---

#### `picocalc.wifi.getIP()`
Returns the current IP address as a string.

- **Parameters:** None
- **Returns:** (string or nil) IP address (e.g., `"192.168.1.100"`), or `nil` if not connected

---

#### `picocalc.wifi.getSSID()`
Returns the SSID of the current connection.

- **Parameters:** None
- **Returns:** (string or nil) SSID, or `nil` if not connected

---

#### `picocalc.wifi.hasInternet()`
Check if the device has working internet connectivity (beyond just WiFi connection).

- **Parameters:** None
- **Returns:** (boolean) `true` if internet is reachable

```lua
if picocalc.wifi.hasInternet() then
    -- Safe to make external requests
end
```

---

### WiFi Status Constants

| Constant | Description |
|----------|-------------|
| `picocalc.wifi.STATUS_DISCONNECTED` | Not connected |
| `picocalc.wifi.STATUS_CONNECTING` | Connection in progress |
| `picocalc.wifi.STATUS_CONNECTED` | Connected successfully |
| `picocalc.wifi.STATUS_FAILED` | Connection failed |
| `picocalc.wifi.STATUS_ONLINE` | Internet connectivity confirmed |

---

## picocalc.network

HTTP client for making network requests. Requires WiFi to be connected first. Up to **8 simultaneous connections** are supported. HTTPS (SSL/TLS) is supported via mbedTLS and **verifies the server
certificate by default** (chain to the OS root bundle, validity dates, host
name). Failures read `"TLS: certificate not trusted …"`, `"… expired"` or
`"… does not match the host name"`. Because validity needs the wall clock, a
verifying connection is refused with an error starting `"clock not set"` until
SNTP has set the time; retry a few seconds later.

HTTP connections are objects with method syntax (`conn:get(...)`, `conn:read()`, etc.). Callbacks are fired automatically — you do not need to poll manually.

### picocalc.network functions

#### `picocalc.network.getStatus()`
Returns the current network status.

- **Parameters:** None
- **Returns:** (number) One of the `kStatus*` constants

```lua
if picocalc.network.getStatus() == picocalc.network.kStatusConnected then
    -- Safe to make HTTP requests
end
```

---

#### `picocalc.network.setEnabled(flag [, callback])`
Enables or disables WiFi. The optional callback is fired **synchronously** with `nil` (reserved for a future async result).

- **Parameters:**
  - `flag` (boolean): `true` to enable, `false` to disable
  - `callback` (function, optional): Called synchronously with `nil`
- **Returns:** None

```lua
picocalc.network.setEnabled(true, function()
    print("WiFi enable requested")
end)
```

---

#### `picocalc.network.isHwDisconnected()`
Check if the WiFi hardware has been disabled (e.g. by video playback).

- **Parameters:** None
- **Returns:** (boolean) `true` if the WiFi hardware is disconnected

```lua
if picocalc.network.isHwDisconnected() then
    print("WiFi hardware is off")
end
```

---

### Network Status Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `picocalc.network.kStatusNotConnected` | 0 | WiFi present but not connected |
| `picocalc.network.kStatusConnected` | 1 | Connected and ready |
| `picocalc.network.kStatusNotAvailable` | 2 | No WiFi hardware, or connection failed |

---

### picocalc.network.http — HTTP Connection Objects

#### `picocalc.network.http.new(server [, port [, usessl]])`
Creates a new HTTP/HTTPS connection object. Does not connect immediately — call `conn:get()` or `conn:post()` to start a request.

- **Parameters:**
  - `server` (string): Hostname (e.g., `"api.example.com"`)
  - `port` (number, optional): TCP port. Defaults to `80` for HTTP, `443` for HTTPS.
  - `usessl` (boolean, optional): `true` to use HTTPS/TLS. Defaults to `false`.
- **Returns:** (userdata) Connection object, or `nil, errstr` if the connection pool is full

```lua
-- Plain HTTP
local conn = picocalc.network.http.new("api.example.com")

-- HTTPS (port 443 is the default when usessl=true)
local conn = picocalc.network.http.new("api.example.com", 443, true)

if not conn then
    print("Pool full")
end
```

---

### HTTP Connection Methods

All methods are called on the connection object with colon syntax.

---

#### `conn:get(path [, headers])`
Issues an HTTP GET request.

Returns `false` ("a request is already in progress") unless the connection is idle or its last request finished. POST bodies are binary-safe.

- **Parameters:**
  - `path` (string): URL path (e.g., `"/api/data"`)
  - `headers` (string or table, optional): Extra headers. Can be a raw `"Key: Value
"` string, a flat array of `"Key: Value"` strings, or a `{key=value}` table.
- **Returns:** (boolean) `true` if request started, or `false, errstr` on immediate failure

```lua
conn:get("/api/data")
```

---

#### `conn:post(path [, headers], data)`
Issues an HTTP POST request.

Returns `false` ("a request is already in progress") unless the connection is idle or its last request finished. POST bodies are binary-safe.

- **Parameters:**
  - `path` (string): URL path
  - `headers` (string or table, optional): Extra headers (omit to pass data as third arg)
  - `data` (string): Request body
- **Returns:** (boolean) `true` if request started, or `false, errstr` on immediate failure

```lua
conn:post("/submit", '{"value":42}')
-- or with headers:
conn:post("/submit", {["Content-Type"] = "application/json"}, '{"value":42}')
```

---

#### `conn:close()`
Closes the connection and frees the connection pool slot. The object is unusable afterwards.

- **Returns:** None

---

#### `conn:setKeepAlive(flag)`
Enables HTTP keep-alive (`Connection: keep-alive`). Must be called before `get`/`post`.

- **Parameters:**
  - `flag` (boolean)
- **Returns:** None

---

#### `conn:setByteRange(from, to)`
Adds a `Range: bytes=from-to` header to the next request (for partial content / resumable downloads). Must be called before `get`/`post`.

- **Parameters:**
  - `from` (number): Start byte (inclusive)
  - `to` (number): End byte (inclusive)
- **Returns:** None

---

#### `conn:setConnectTimeout(seconds)`
Sets the TCP connection timeout. Default is 10 seconds.

- **Parameters:**
  - `seconds` (number): Timeout in seconds (fractions allowed)
- **Returns:** None

---

#### `conn:setReadTimeout(seconds)`
Sets the timeout waiting for response data after connecting. Default is 30 seconds.

- **Parameters:**
  - `seconds` (number): Timeout in seconds (fractions allowed)
- **Returns:** None

---

#### `conn:setReadBufferSize(bytes)`
Resizes the receive ring buffer. Must be called before `get`/`post`. Defaults to 4096 bytes. Maximum is 2097152 bytes (2 MiB).

- **Parameters:**
  - `bytes` (number): Buffer size in bytes
- **Returns:** None

---

#### `conn:setInsecure(flag)`
Before `get`/`post`: `true` skips certificate verification and the clock check
for this connection only (self-signed development servers). Logs a warning.

---

#### `conn:getError()`
Returns the last error string, if any.

- **Returns:** (string or nil) Error description, or `nil` if no error

```lua
local err = conn:getError()
if err then print("HTTP error: " .. err) end
```

---

#### `conn:getProgress()`
Returns download progress.

- **Returns:** (number, number) `bytesReceived, totalBytes` — `totalBytes` is `-1` if unknown (headers not yet received, no `Content-Length` header or chunked encoding)

This differs from the native `http->getProgress(conn, &received, &total)`, which writes both values through pointers and *returns only the total*. See [Lua and native HTTP](#lua-and-native-http).

```lua
local received, total = conn:getProgress()
if total > 0 then
    print(string.format("%d%%", received * 100 // total))
end
```

---

#### `conn:getBytesAvailable()`
Returns the number of bytes currently available to read from the receive buffer.

- **Returns:** (number) Bytes available

---

#### `conn:read([length])`
Reads data from the receive buffer.

- **Parameters:**
  - `length` (number, optional): Maximum bytes to read. Defaults to all available. Capped at 131072 bytes per call.
- **Returns:** (string or nil) Data, or `nil` if nothing is available yet

```lua
local chunk = conn:read()
if chunk then buffer = buffer .. chunk end
```

---

#### `conn:getResponseStatus()`
Returns the HTTP response status code.

- **Returns:** (number or nil) e.g. `200`, `404` — `nil` if headers not yet received

---

#### `conn:getResponseHeaders()`
Returns the response headers as a key-value table (lowercase keys).

- **Returns:** (table or nil) `{["content-type"]="text/json", ...}` — `nil` if headers not yet received

```lua
local hdrs = conn:getResponseHeaders()
if hdrs then
    print("Content-Type: " .. (hdrs["content-type"] or "unknown"))
end
```

---

### Responses

Content-Length, chunked and close-delimited bodies are supported, with keep-alive reuse. A body cut short (reset, early close, out of memory) fails; it never reports complete. Callbacks never nest: a callback that sleeps leaves later events for the outer call. The read timeout runs from the moment the request is sent.

### Callbacks

Callbacks are fired automatically from within the app's execution loop — no polling needed.

#### `conn:setRequestCallback(fn)`
Called each time new body data arrives. `fn` receives no arguments; use `conn:read()` to consume data.

#### `conn:setHeadersReadCallback(fn)`
Called once when all response headers have been received. Use `conn:getResponseStatus()` and `conn:getResponseHeaders()` inside this callback.

#### `conn:setRequestCompleteCallback(fn)`
Called when the response body is fully received. This is the Lua replacement for the native `http->isComplete()`; the connection stays open afterwards, so you can still `read()` what is buffered, then call `conn:close()`.

#### `conn:setConnectionClosedCallback(fn)`
Called when the request fails (connect error, timeout, reset, a body cut short, out of memory). It does not fire after a successful response. Read `conn:getError()` inside the callback: once it returns, the connection is released (`conn:get`, `post` and the `set*` methods raise `http: connection is closed`; `getError`, `getResponseStatus`, `getResponseHeaders` and `read` return `nil`, `getBytesAvailable` returns `0`, `getProgress` returns `0, -1`).

---

### Lua and native HTTP

The native `picocalc_http_t` (`sdk/native/os.h`) is polled: call `http->get()`, then each frame test `http->isComplete()`, drain with `available()`/`read()`, and read the outcome with `getStatus()`/`getError()`. Native apps get no callbacks. Lua has no `isComplete()`; completion arrives through `setRequestCompleteCallback`, and failure through `setConnectionClosedCallback`. Callbacks fire from `picocalc.sys.sleep` and the app's instruction hook, so a loop that calls `sleep` sees them.

| Native | Lua |
|--------|-----|
| `newConn(server, port, use_ssl)` (`port` 0 = default) | `http.new(server [, port [, usessl]])` |
| `get`/`post` return nothing | `conn:get`/`conn:post` return `true`, or `false, errstr` |
| `available(c)` | `conn:getBytesAvailable()` |
| `read(c, buf, len)` returns the byte count (0 if none) | `conn:read([length])` returns a string, or `nil` when empty |
| `getStatus(c)` (0 = not yet) | `conn:getResponseStatus()` (`nil` = not yet) |
| `getProgress(c, &received, &total)` returns `total` | `conn:getProgress()` returns `received, total` |
| `isComplete(c)` | none: use `setRequestCompleteCallback` |

Native-only, with no Lua equivalent: `isComplete`. Lua-only: `getResponseHeaders`, the four `set*Callback` methods.

A polling flow in Lua:

```lua
local conn = picocalc.network.http.new("httpbin.org")
local parts, done, failure = {}, false, nil

conn:setRequestCompleteCallback(function() done = true end)
conn:setConnectionClosedCallback(function() failure = conn:getError() or "closed" end)

if not conn:get("/bytes/2048") then return end

while not done and not failure do
    -- Drain whatever has arrived.
    while conn:getBytesAvailable() > 0 do
        parts[#parts + 1] = conn:read()
    end
    local received, total = conn:getProgress()   -- total is -1 while unknown
    picocalc.input.update()
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then
        conn:close()
        return
    end
    picocalc.sys.sleep(16)   -- fires the callbacks
end

if failure then
    print("HTTP failed: " .. failure)   -- the connection is already released
    return
end

-- Body bytes that arrived after the last drain are still buffered.
while conn:getBytesAvailable() > 0 do parts[#parts + 1] = conn:read() end
print(conn:getResponseStatus(), #table.concat(parts))
conn:close()
```

---

### HTTP Example

Check that WiFi is up, then run the polling flow from [Lua and native HTTP](#lua-and-native-http):

```lua
if picocalc.network.getStatus() ~= picocalc.network.kStatusConnected then
    print("Not connected")
    return
end
```

Alternatively, drain the body in `setRequestCallback` instead of the loop:

```lua
local conn = picocalc.network.http.new("httpbin.org")
local body, done, failure = "", false, nil

conn:setHeadersReadCallback(function()
    print("Status: " .. conn:getResponseStatus())
end)
conn:setRequestCallback(function()
    local chunk = conn:read()
    if chunk then body = body .. chunk end
end)
conn:setRequestCompleteCallback(function() done = true end)
conn:setConnectionClosedCallback(function()
    failure = conn:getError() or "closed"
end)

if not conn:get("/get") then return end

while not done and not failure do
    picocalc.input.update()
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then
        conn:close()
        return
    end
    picocalc.sys.sleep(16)   -- fires the callbacks
end

if failure then
    print("Error: " .. failure)   -- the connection is already released
else
    print("Done. Body length: " .. #body)
    conn:close()
end
```
