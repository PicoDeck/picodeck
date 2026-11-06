---
title: "Library Download"
---

Stream an HTTP(S) response straight to a file on the SD card. The library wraps `picocalc.network.http` in a single blocking call with redirect handling, progress reporting and automatic cleanup of partial files, so an app can fetch an asset pack or an update with one function call. A file already at the destination is replaced only once the new one is complete.

The library's source is [`system/lib/download.lua`](https://github.com/PicoDeck/picodeck/blob/master/system/lib/download.lua) in the repo; it must be on the SD card at `/system/lib/download.lua`. Load it with:

```lua
local download = picocalc.sys.loadlib("download")
```

An app you publish should not rely on that: [bundle your own copy](Publishing-Apps.md#bundle-the-libraries-you-use) and `require` it.

Your app needs the `"http"` requirement in `app.json` and a connected network.

---

## `download.toFile(url, dest [, opts])`

Blocking entry point. Returns only when the transfer has finished, failed, or timed out.

- **Parameters:**
  - `url` (string): An `http://` or `https://` URL. A `:port` suffix on the host is honoured.
  - `dest` (string): Destination file path. Parent directories are created as needed. Must be writable by the app (its `/data/<APP_ID>` directory, unless `root-filesystem` is granted).
  - `opts` (table, optional): See [Options](#options)
- **Returns:** (boolean [, string]) `true` on success, or `false, errorString` on failure: `"invalid URL: …"`, `"HTTP <status>"` (anything but 200, so also a 206), `"empty response"` (a 200 with no body: an empty file cannot be downloaded), `"redirect without Location"`, `"too many redirects"`, `"timeout"`, `"cannot open <dest>"`, `"SD write failed (card full?)"`, `"SD card not responding"`, or the connection's own error (a refused connection, a certificate failure, `"clock not set…"`, where a retry a few seconds later works; see [`conn:getError()`](API-Network-and-WiFi.md#conngeterror))

```lua
local download = picocalc.sys.loadlib("download")

local ok, err = download.toFile(
    "https://example.com/packs/levels.zip",
    picocalc.fs.appPath("levels.zip"),
    {
        onProgress = function(received, total)
            -- total is 0 until the end when the server sent no Content-Length
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
| `onProgress` | function | (none) | `function(received, total)` called as chunks arrive (up to 32 KB at a time) and once more on completion. `total` is `0` while unknown (no `Content-Length` header); the final call passes `received` as `total`. Errors raised inside the callback are swallowed (`pcall`). |
| `headers` | table | (none) | Extra request headers, merged over the default `User-Agent: PicoDeck-download/1.0`. |
| `timeoutMs` | number | `120000` | Whole-transfer deadline in milliseconds, spanning all redirect hops. |
| `maxRedirects` | number | `5` | How many 3xx redirects to follow before giving up. |

---

## Behaviour

### Blocking contract

`toFile` runs its own wait loop until an HTTP callback resolves the transfer, so nothing else in your app executes during the download, apart from `onProgress` and any callbacks of your own. Use `onProgress` to keep the screen alive; drawing and flushing the display from the callback is fine. The system menu and Exit App work during a download.

Network callbacks never nest, so `toFile` must be called from ordinary app code, **never from inside another network callback**: the download's own callbacks could not fire, and it would wait out its timeout and fail.

### Streaming and slow mode

Response chunks are written to the SD card as they arrive, so memory use does not grow with the file; a 50 MB download needs no more RAM than a 50 KB one. It needs a 256 KB receive ring (32 KB when the heap cannot spare that, which also means less margin while the card catches up) plus up to 64 KB at a time for each read: 32 KB of temporary buffer and a 32 KB string.

For the duration of the transfer, writes happen under `fs.setSlowMode(true)`: while the CYW43 radio is active, its EMI corrupts 25 MHz SD writes, so SPI0 drops to 1 MHz until the transfer ends. This makes downloads SD-bound and noticeably slower than plain file writes; that is the price of writing safely while the radio is on, not a bug.

### Partial files and an existing destination

The body goes to `dest .. ".part"` first, in the same directory. Only when it is all on the card does that file take `dest`'s place.

- On any failure (connection error, non-200 status, timeout, SD failure), the `.part` file is deleted before `toFile` returns. `false, err` therefore means nothing new was left behind, and a file that was already at `dest` is exactly as it was.
- `true` means the file at `dest` is complete: every byte received reached the card, and it is the size the server said. A short write on a full card is a failure. To verify integrity beyond that, hash the result with `picocalc.crypto.sha256File(dest)` and compare against a published checksum.
- If the app exits mid-download the `.part` file stays, and the next `toFile` to that `dest` removes it. While an old file is being replaced it is briefly named `dest .. ".old"`, so a power cut at that moment leaves it there.

### Redirects

3xx responses with a `Location` header are followed, up to `maxRedirects` hops (default 5), all within the single `timeoutMs` budget. `Location` may be a full URL or relative to the URL that sent it (`/new/path`, `new`, `../new`, `//host/path`). Any bytes written during a redirected leg are removed before the next hop starts.

---

## Requirements and limits

- The app needs `"http"` in its `app.json` requirements, and the network must be connected (`picocalc.network.getStatus() == picocalc.network.kStatusConnected`).
- GET requests only.
- Fixed per-leg tuning: 30 s connect timeout, 60 s read timeout, 256 KB receive ring.
- Must be called from plain Lua code, not from inside another network callback.
