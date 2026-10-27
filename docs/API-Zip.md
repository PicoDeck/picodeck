---
title: "API Zip"
---

Read and extract ZIP archives on the SD card. Useful for shipping an app's assets as a single file: transfer one archive instead of hundreds of loose files, then extract it once, or open it and read entries straight out of it at runtime.

## picocalc.zip

### Functions

#### `picocalc.zip.list(zip_path)`
List the file entries of a ZIP archive (directory entries are skipped).

- **Parameters:**
  - `zip_path` (string): Path to the ZIP file
- **Returns:** (table or nil, string) Array of `{name, size, compressed_size}` entries, or `nil, errorString` on failure

```lua
local entries, err = picocalc.zip.list("/apps/mygame/assets.zip")
if entries then
    for _, e in ipairs(entries) do
        picocalc.sys.log(e.name .. " (" .. e.size .. " bytes)")
    end
end
```

---

#### `picocalc.zip.extract(zip_path, dest_dir [, progress_fn])`
Extract every file in a ZIP archive to a directory. Parent directories are created as needed. Entries with unsafe names are skipped rather than failing the whole extraction (see [Limits and name validation](#limits-and-name-validation)).

- **Parameters:**
  - `zip_path` (string): Path to the ZIP file
  - `dest_dir` (string): Destination directory
  - `progress_fn` (function, optional): Progress callback receiving `(done, total)` after each extracted file
- **Returns:** (boolean, string) `true` on success, or `false, errorString` on failure

```lua
local ok, err = picocalc.zip.extract("/apps/mygame/assets.zip", "/data/com.example.mygame/assets",
    function(done, total)
        picocalc.sys.log("extracting " .. done .. "/" .. total)
    end)
if not ok then
    picocalc.sys.log("extract failed: " .. err)
end
```

---

#### `picocalc.zip.open(path)`
Open an archive for random access without extracting it. Entries can then be listed, tested, read into memory, or streamed to files through the returned archive object.

At most **4 archives** may be open at once per app. An archive is closed by `:close()`, by the garbage collector, when it leaves a to-be-closed scope (`local ar <close> = ...`), and automatically when the app exits.

- **Parameters:**
  - `path` (string): Path to the ZIP file
- **Returns:** (userdata or nil, string) Archive object, or `nil, errorString` on failure
- **Errors:** `"permission denied"`, `"too many open archives (max 4)"`, or `"open failed"` (the file cannot be opened) or `"bad zip"` (not a valid archive). (`picocalc.zip.list` opens an archive only for the call and does not count towards the 4.)

```lua
local ar, err = picocalc.zip.open(APP_DIR .. "/assets.zip")
if not ar then error(err) end
```

---

### Archive Methods

The archive object returned by `picocalc.zip.open()` is the Lua surface for random access. Entries can be addressed **by name** (`exists`, `size`, `read`, `extract`) or **by index** (`numEntries`, `locate`, `statIndex`, `extractEntry`) — the index forms mirror the C API, so a port can keep its indexes; `ar:read` returns a string rather than filling a caller buffer. See [the C-to-Lua mapping](#c-to-lua-mapping). Use it as `ar:method(...)`.

Errors come in two kinds. A **runtime failure** (missing entry, bad archive, permission, size cap) is reported in the return values, per method: `ar:read` gives `nil, errorString`, `ar:extract`, `ar:extractEntry` and `ar:extractAll` give `false, errorString`, `ar:exists` gives `false`, and `ar:size`, `ar:locate` and `ar:statIndex` give a bare `nil`; `ar:list` cannot fail. **Misuse** raises a Lua error: calling any method on a closed archive raises `archive is closed` (except `:close()`, which is a no-op when already closed), and a missing or wrongly typed argument raises the usual argument error.

#### `ar:list()`
List the archive's file entries (directory entries are skipped). Same result shape as `picocalc.zip.list`.

- **Parameters:** None
- **Returns:** (table) Array of `{name, size, compressed_size}` entries

```lua
for _, e in ipairs(ar:list()) do
    picocalc.sys.log(e.name)
end
```

---

#### `ar:exists(name)`
Check whether an entry with this exact name exists. Names include any directory prefix inside the archive (for example `"images/hero.png"`).

- **Parameters:**
  - `name` (string): Entry name
- **Returns:** (boolean) `true` if the entry exists (directory entries included), `false` otherwise. Never returns an error string.

```lua
if ar:exists("images/hero.png") then ... end
```

---

#### `ar:size(name)`
Get an entry's uncompressed size.

- **Parameters:**
  - `name` (string): Entry name
- **Returns:** (number or nil) Uncompressed size in bytes, or `nil` (no error string) if the entry does not exist

```lua
local bytes = ar:size("music/theme.mod")
```

---

#### `ar:numEntries()`
How many entries the archive holds: files **and** directory entries.

- **Returns:** (number) Entry count
- **Errors:** raises `archive is closed` on a closed archive

Unlike `ar:list()`, which skips directory entries, this counts them — it is the
loop bound for walking an archive by index. Indexes are **0-based, as in the C
API**: the valid indexes are `0 .. numEntries() - 1`. (`ar:list()` returns a
1-based Lua array, so do not reuse its loop bounds.)

```lua
for i = 0, ar:numEntries() - 1 do
    local st = ar:statIndex(i)
    if not st.is_dir then print(i, st.name, st.size) end
end
```

---

#### `ar:locate(name)`
The index of an entry, for the index-addressed calls below. Indexes are 0-based, as in the C API (the first entry is `0`).

- **Parameters:**
  - `name` (string): Entry name
- **Returns:** (number or nil) Entry index, or `nil` if no entry has that exact name

```lua
local i = ar:locate("music/theme.mod")
if i then
    local st = ar:statIndex(i)
    print(st.size, "bytes, compressed to", st.compressed_size)
end
```

---

#### `ar:statIndex(index)`
One entry's metadata, addressed by index.

- **Parameters:**
  - `index` (number): Entry index, **0-based, as in the C API**: valid indexes are `0 .. numEntries() - 1`. Must be an integer (a fractional value raises)
- **Returns:** (table or nil) A table with `name` (string), `size` (uncompressed bytes), `compressed_size` (bytes) and `is_dir` (boolean) — the same keys `ar:list()` reports — or `nil` (no error string) for an index outside `0 .. numEntries() - 1`
- **Errors:** raises `archive is closed` on a closed archive

Directory entries are included here, unlike in `ar:list()`.

---

#### `ar:extractEntry(index, dest_path)`
`ar:extract()` addressed by entry index instead of name. Constant memory, and
the destination is caller-chosen as always.

- **Parameters:**
  - `index` (number): Entry index, **0-based, as in the C API** (`0 .. numEntries() - 1`). Must be an integer
  - `dest_path` (string): Destination file path (subject to the write sandbox)
- **Returns:** (boolean, string) `true` on success, or `false, errorString`
- **Errors:** raises `archive is closed` on a closed archive; other failures come back as `false, error`. An index outside `0 .. numEntries() - 1` returns `false, "no such entry"` **before** the destination is opened, so an existing file at `dest_path` is left untouched

---

#### `ar:read(name [, max_len])`
Decompress a whole entry into a Lua string, without touching the SD card's filesystem. Fails if the entry's uncompressed size exceeds `max_len` (when given) or the 4 MB in-memory cap. The size is checked before anything is decompressed, so a rejected entry costs no memory.

- **Parameters:**
  - `name` (string): Entry name
  - `max_len` (number, optional): Reject entries that decompress to more than this many bytes. `0`, a negative value or `nil` means no limit beyond the 4 MB cap; a larger value never raises the cap.
- **Returns:** (string or nil, string) Entry contents (a zero-length entry gives the empty string), or `nil, errorString` on failure
- **Errors:** `"no such entry"`, `"size cap"` (larger than `max_len` or 4 MB), `"bad zip"`, or an engine message such as `"extract failed"` (corrupt or unsupported data). Out of memory raises.

```lua
local data, err = ar:read("levels/level1.json")
if data then
    local level = picocalc.json.decode(data)
end
```

---

#### `ar:extract(name, dest_path)`
Stream one entry to a file on the SD card, using constant memory regardless of entry size. The entry name only selects the data; the destination path is entirely caller-chosen (subject to the write sandbox), so no entry-name validation applies here.

- **Parameters:**
  - `name` (string): Entry name
  - `dest_path` (string): Destination file path (parent directories are created as needed)
- **Returns:** (boolean, string) `true` on success, or `false, errorString` on failure
- **Errors:** `"no such entry"`, `"permission denied (destination)"`, or an engine message (`"mkdir failed"`, `"extract failed"`, ...). A failed extraction removes the partial file.

```lua
local ok, err = ar:extract("music/theme.mod", "/data/com.example.mygame/theme.mod")
```

---

#### `ar:extractAll(dest_dir [, progress_fn])`
Extract every file entry into a directory. Same behaviour as `picocalc.zip.extract`, reusing the already-open handle.

- **Parameters:**
  - `dest_dir` (string): Destination directory
  - `progress_fn` (function, optional): Progress callback receiving `(done, total)` after each extracted file
- **Returns:** (boolean, string) `true` on success, or `false, errorString` on failure
- **Errors:** `"permission denied (destination)"` or an engine message (entry-count or total-size cap exceeded, write failure, ...). Errors raised inside `progress_fn` are ignored; a `sys.exit()` from it stops the extraction and the app exits.

```lua
local ok, err = ar:extractAll("/data/com.example.mygame/assets")
```

---

#### `ar:close()`
Close the archive and release its SD file handle. Closing an already-closed archive is a no-op. Also called automatically by the garbage collector and on `<close>` scope exit.

- **Parameters:** None
- **Returns:** None

```lua
ar:close()
```

---

### C-to-Lua mapping

The native `g_api.zip` (see [Native API](#native-api-c)) and the Lua archive object cover the same ground with two shapes: both address entries by **index** (`numEntries`/`locate`/`statIndex`/`extractEntry`) and both by **name** (`exists`/`size`/`read`/`extract`). C fills a buffer you allocate; Lua returns a string. Porting either way:

| C (`api->zip->...`) | Lua | Note |
|---|---|---|
| `extract(zip_path, dest_dir)` | `picocalc.zip.extract(zip_path, dest_dir [, progress])` | Lua adds a progress callback and an error string |
| `list(zip_path)` (count) | `picocalc.zip.list(zip_path)` (table) | Lua returns the entries, files only |
| `open(path)` (`NULL` on error) | `picocalc.zip.open(path)` (`nil, err`) | at most 4 open per app in both |
| `numEntries(z)` | `ar:numEntries()` | Same |
| `locate(z, name)` (index or -1) | `ar:locate(name)` | Same, but Lua returns `nil` rather than `-1`. `ar:exists(name)` is the boolean form |
| `statIndex(z, idx, &st)` | `ar:statIndex(idx)` | Same fields; Lua returns a table or `nil` instead of a bool out-param |
| `read(z, idx, buf, cap)` | `ar:read(name [, max_len])` | Lua allocates the string; there is no undersized-buffer case (hence no `PCZIP_ERR_TOO_SMALL`) |
| `extractEntry(z, idx, dest)` | `ar:extractEntry(idx, dest)` | Same |
| (none) | `ar:size(name)` / `ar:read(name)` / `ar:extract(name, dest)` | Name-addressed convenience over the index calls |
| (none) | `ar:extractAll(dest [, progress])` | C: use `extract` |
| `close(z)` | `ar:close()` | Lua also closes on GC, `<close>` and app exit |

---

### Example

An asset bundle: `assets.zip` sits next to `main.lua` and holds the app's images. Open the archive once at app start, decode images on demand straight from it (no extraction to SD), and close it at exit.

```lua
local image = picocalc.graphics.image

-- Open once at app start
local ar = assert(picocalc.zip.open(APP_DIR .. "/assets.zip"))

-- Read entries on demand; loadFromBuffer decodes from memory
local function loadSprite(name)
    local data, err = ar:read("images/" .. name)
    if not data then return nil, err end
    return image.loadFromBuffer(data)
end

local hero = assert(loadSprite("hero.png"))

-- ... main loop ...

-- Release the SD file handle at exit
ar:close()
```

Keep the archive open for the app's lifetime instead of re-opening it per read: `open` pays the central-directory walk once, and each later read seeks straight to its entry.

---

### Sandbox

All paths respect the app sandbox:

- `picocalc.zip.open`, `picocalc.zip.list` and the `zip_path` of `picocalc.zip.extract` need **read** access to the archive.
- The destinations of `picocalc.zip.extract`, `ar:extract` and `ar:extractAll` need **write** access.

By default an app can read `/apps/<dirname>` and read/write `/data/<APP_ID>`; the `root-filesystem` requirement in `app.json` lifts this to the whole SD card.

---

### Limits and name validation

Hard caps, enforced by the shared ZIP engine:

| Limit | Value |
|-------|-------|
| Entries per archive | 8192 |
| Total uncompressed size | 256 MB |
| Single in-memory read (`ar:read`) | 4 MB |
| Entry name length | 255 characters |

`picocalc.zip.extract` and `ar:extractAll` check the entry-count and total-uncompressed-size caps up front and fail fast, before writing anything (zip-bomb protection).

Entry names are strictly validated before an entry name may become a filesystem path. Rejected: leading `/`, any backslash or `:`, control bytes, and any path component that is exactly `.` or `..` (a name like `foo..bar` is fine). During `extractAll`, entries with invalid names are skipped and counted; they do not fail the extraction. `ar:extract(name, dest)` skips this check because the entry name never becomes a path there.

---

### Performance notes

- The reader is seek-based: the archive is streamed from the SD card, so opening costs one pass over the central directory, not the archive size, and the archive is never copied to the heap.
- Extraction (`extract`, `ar:extract`, `ar:extractAll`) streams with constant memory, whatever the entry size.
- The SD bus is shared with Core 1 audio (MP3 and file player). Pace big `ar:read` calls: decompressing a multi-megabyte entry holds the SD bus long enough to starve audio decode, so do large reads at load screens, not mid-gameplay.

---

### Native API (C)

`g_api.zip` (`picocalc_zip_t` in `src/os/os.h`). `extract(zip_path, dest_dir)` and `list(zip_path)` exist since Phase 2. API version 5 (`g_api.version >= 5`) appends read-in-place archive handles: `open`, `close`, `numEntries`, `locate`, `statIndex`, `read` and `extractEntry`, working on an opaque `pczip_t` handle. Check `api->version >= 5` before calling them; on older firmware the struct ends at `list`.

Reads are caller-allocated: `statIndex` first to size the buffer (`st.size` is the uncompressed size), then `read` decompresses into it. `read` returns the number of bytes written, or a negative code:

| Return | Meaning |
|---|---|
| `>= 0` | bytes written (0 for an empty entry) |
| `PCZIP_ERR_TOO_SMALL` (-2) | the entry is larger than `buf_cap`; nothing was written. Allocate `st.size` bytes and retry |
| `-1` | any other error: bad handle, bad index, corrupt data |

Test the result with `< 0`, never `== -1`: both codes are negative, so a caller that only checks `n < 0` is safe with either. `PCZIP_ERR_TOO_SMALL` is defined in `os.h` and needs no API version bump; firmware from before it returned `-1` for the too-small case as well, so code that must run there should size the buffer from `statIndex` and treat every negative result as a failure.

```c
pczip_t z = api->zip->open("/apps/mygame/assets.zip");   // NULL on error
if (!z) return;

int idx = api->zip->locate(z, "images/hero.png");        // -1 if absent
pczip_stat_t st;
if (idx >= 0 && api->zip->statIndex(z, idx, &st)) {
    void *buf = umm_malloc(st.size);                     // caller allocates
    if (buf) {
        int got = api->zip->read(z, idx, buf, st.size);  // bytes written, < 0 on error
        if (got >= 0) {
            // ... use buf[0..got) ...
        }
        umm_free(buf);
    }
}

// Or stream an entry to the SD card with constant memory:
api->zip->extractEntry(z, idx, "/data/com.example.mygame/hero.png");

api->zip->close(z);
```

The same limits apply as in Lua: at most 4 open handles per app (force-closed when the app exits), 8192 entries, 256 MB total uncompressed. `pczip_stat_t` carries `name`, `size` (uncompressed), `comp_size` and `is_dir`; `numEntries` counts files plus directories, so skip `is_dir` entries when iterating file content.
