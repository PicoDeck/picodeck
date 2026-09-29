---
title: "Library Download"
---

Stream an HTTP(S) response straight to a file on the SD card. The library wraps `picocalc.network.http` in a single blocking call with redirect handling, progress reporting and automatic cleanup of partial files, so an app can fetch an asset pack or an update with one function call.

The library's source is `system/lib/download.lua` in the repo; it must be on the SD card at `/system/lib/download.lua`. Load it with:

```lua
local download = picocalc.sys.loadlib("download")
```

Your app needs the `"http"` requirement in `app.json` and a connected network.

---

## `download.toFile(url, dest [, opts])`

Blocking entry point. Returns only when the transfer has finished, failed, or timed out.

- **Parameters:**
  - `url` (string): An `http://` or `https://` URL. A `:port` suffix on the host is honoured.
  - `dest` (string): Destination file path. Parent directories are created as needed. Must be writable by the app (its `/data/<APP_ID>` directory, unless `root-filesystem` is granted).
  - `opts` (table, optional): See [Options](#options)
- **Returns:** (boolean [, string]) `true` on success, or `false, errorString` on failure (invalid URL, connection error, non-200 status, timeout, SD failure, too many redirects)

```lua
local download = picocalc.sys.loadlib("download")

local ok, err = download.toFile(
    "https://example.com/packs/levels.zip",
    "/data/com.example.mygame/levels.zip",
    {
        onProgress = function(received, total)
            -- total is 0 when the server sent no Content-Length
            drawProgressBar(received, total)
        end,
        timeoutMs = 120000,
    })
if not ok then
    picocalc.sys.log("download failed: " .. err)
end
```

### Options

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `onProgress` | function | (none) | `function(received, total)` called as chunks arrive and once more on completion. `total` is `0` when unknown (no `Content-Length` header). Errors raised inside the callback are swallowed (`pcall`). |
| `headers` | table | (none) | Extra request headers, merged over the default `User-Agent: PicoDeck-download/1.0`. |
| `timeoutMs` | number | `120000` | Whole-transfer deadline in milliseconds, spanning all redirect hops. |
| `maxRedirects` | number | `5` | How many 3xx redirects to follow before giving up. |

---

## Behaviour

### Blocking contract

`toFile` runs its own wait loop until an HTTP callback resolves the transfer, so nothing else in your app executes during the download. Use `onProgress` to keep the screen alive; drawing and flushing the display from the callback is fine.

HTTP callbacks are fired from the Lua debug hook, which only runs while plain Lua code is executing. `toFile` must therefore be called from ordinary app code, **never from inside another network callback**: the hook does not re-enter, so a nested download would wait forever and then time out.

### Streaming and slow mode

Response chunks are written to the SD card as they arrive, so peak memory stays at one read buffer (32 KB) regardless of file size; a 50 MB download needs no more RAM than a 50 KB one.

For the duration of the transfer, writes happen under `fs.setSlowMode(true)`: while the CYW43 radio is active, its EMI corrupts 25 MHz SD writes, so SPI0 drops to 1 MHz until the transfer ends. This makes downloads SD-bound and noticeably slower than plain file writes; that is the price of writing safely while the radio is on, not a bug.

### Partial-file cleanup

On any failure (connection error, non-200 status, timeout, SD failure), the partially written file is deleted before `toFile` returns. `false, err` therefore means no file was left behind, and `true` means the file is complete. To verify integrity, hash the result with `picocalc.crypto.sha256File(dest)` and compare against a published checksum.

### Redirects

3xx responses with a `Location` header are followed, up to `maxRedirects` hops (default 5), all within the single `timeoutMs` budget. Any bytes written during a redirected leg are removed before the next hop starts.

---

## Requirements and limits

- The app needs `"http"` in its `app.json` requirements, and the network must be connected (`picocalc.network.getStatus() == picocalc.network.kStatusConnected`).
- GET requests only.
- Fixed per-leg tuning: 30 s connect timeout, 60 s read timeout, 32 KB read buffer.
- Must be called from plain Lua code, not from inside another network callback.
