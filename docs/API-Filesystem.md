---
title: "API Filesystem"
---

Filesystem access to the SD card (FAT32).

## picocalc.fs

### Functions

#### `picocalc.fs.open(path [, mode])`
Opens a file on the SD card.

- **Parameters:**
  - `path` (string): Absolute file path. The parent directory must exist.
  - `mode` (string, optional): File mode, see below. Defaults to `"r"`.
- **Returns:** a file handle (userdata), or `nil, err` (`"permission denied"`, `"cannot open file"`, `"too many open files"`). A 17th open file reads `"cannot open file"`.

Modes are not C's `fopen` modes. Only the letters `r`, `w` and `a` count; `b` and `+` change nothing:

| Mode | Reads | Writes | Missing file | Existing file |
|---|---|---|---|---|
| `"r"`, `"rb"`, `"r+"` | Yes | No | Fails | Kept |
| `"w"`, `"wb"`, `"w+"` | No | Yes | Created | Emptied |
| `"a"`, `"ab"`, `"a+"` | No | Yes | Created | Kept; writing starts at the end |
| `"rw"` | Yes | Yes | Created | Kept |

A file open for writing cannot be opened a second time, and a file that is open in any mode cannot be opened for writing, renamed or deleted, until it is closed.

Handles have methods — `f:read(n)`, `f:write(s)`, `f:seek(offset [, whence])`, `f:tell()`,
`f:close()` — identical to the `picocalc.fs.*` functions. A handle closes
itself when garbage-collected, and `local f <close> = picocalc.fs.open(...)`
closes it at the end of the scope. Using a closed handle raises "attempt to
use a closed file"; `close` is idempotent. At most 16 files can be open at
once (FatFS), and files still open when the app exits are closed by the OS.

```lua
local f = picocalc.fs.open("/data/save.txt", "w")
if f then
    picocalc.fs.write(f, "Hello")
    picocalc.fs.close(f)
end
```

---

#### `picocalc.fs.read(file, length)`
Reads bytes from an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `length` (number): Number of bytes to read
- **Returns:** (string or nil) Data read. `nil` at end of file, for a length of 0, and on an error (including a handle not open for reading).

Raises on a negative length or a closed handle; the length is clamped to the bytes left in the file.

```lua
local data = picocalc.fs.read(f, 1024)
```

---

#### `picocalc.fs.write(file, data)`
Writes data to an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `data` (string): Data to write
- **Returns:** (number) Number of bytes written, or `-1` on an error (including a handle not open for writing).

A full card is not an error: the call returns a smaller count. Compare it with the length of your data whenever losing the end of a file matters. See [Saving a file safely](#saving-a-file-safely).

```lua
local n = picocalc.fs.write(f, "content")
```

---

#### `picocalc.fs.close(file)`
Closes an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
- **Returns:** None

Closing flushes the file to the card. It reports nothing, so a flush that failed shows only as a size smaller than you wrote: `picocalc.fs.size(path)` after closing tells you.

---

#### `picocalc.fs.exists(path)`
Checks if a file or directory exists.

- **Parameters:**
  - `path` (string): Absolute path
- **Returns:** (boolean) `true` if exists, `false` otherwise

```lua
if picocalc.fs.exists("/data/save.txt") then
    -- Load saved data
end
```

---

#### `picocalc.fs.readFile(path)`
Reads an entire file into memory in one call.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (string or nil) File contents, or `nil` when the path is denied, missing or unreadable. A file larger than free memory **raises** a memory error instead of returning `nil`; wrap it in `pcall` when the size is not known to fit.

```lua
local content = picocalc.fs.readFile("/apps/hello/config.txt")
```

---

#### `picocalc.fs.size(path)`
Returns the size of a file in bytes.

- **Parameters:**
  - `path` (string): Absolute file path
- **Returns:** (number) File size in bytes, or `-1` on error (file not found or sandbox violation)

```lua
local bytes = picocalc.fs.size("/data/save.txt")
if bytes >= 0 then
    print("File is " .. bytes .. " bytes")
end
```

---

#### `picocalc.fs.isDir(path)`
Whether a path exists and is a directory.

- **Parameters:**
  - `path` (string): Absolute path
- **Returns:** (boolean) `true` only when the path exists **and** is a directory (the volume root `/` is one, for an app with the `root-filesystem` requirement; `fs.exists("/")` and `fs.stat("/")` agree, the latter with `size` 0 and no timestamp). `false` for a file, for a missing path, and for a path the sandbox refuses — the same "not usable" answer `fs.exists` gives.
- **Errors:** none

```lua
if picocalc.fs.isDir("/apps/mygame/assets") then
    for _, entry in ipairs(picocalc.fs.listDir("/apps/mygame/assets")) do
        print(entry.name, entry.size)
    end
end
```

---

#### `picocalc.fs.listDir(path)`
Lists the contents of a directory.

- **Parameters:**
  - `path` (string): Absolute directory path
- **Returns:** (table) Array of entries, in the order they sit in the directory. An empty table when the directory cannot be read, plus `"permission denied"` as a second value when the sandbox refused it. Each entry is a table with:
  - `name` (string): File or directory name, not the full path
  - `is_dir` (boolean): `true` if directory, `false` if file
  - `size` (number): File size in bytes (0 for directories)

```lua
local entries = picocalc.fs.listDir("/apps")
for _, e in ipairs(entries) do
    print(e.name, e.is_dir, e.size)
end
```

---

#### `picocalc.fs.mkdir(path)`
Creates one directory. Its parent must exist; to make a chain, create each level in turn.

- **Parameters:**
  - `path` (string): Absolute directory path to create
- **Returns:** (boolean) `true` if successful or directory already exists, `false` on error

```lua
-- Create app data directory
local data_dir = "/data/" .. APP_ID
if picocalc.fs.mkdir(data_dir) then
    print("Data directory ready")
end
```

---

#### `picocalc.fs.seek(file, offset [, whence])`
Moves the read/write position within an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
  - `offset` (number): Byte offset, relative to `whence`
  - `whence` (string, optional): `"set"` (default) counts from the start of the file and `offset` must be `>= 0`; `"cur"` counts from the current position; `"end"` counts from the end of the file. `"cur"` and `"end"` accept negative offsets.
- **Returns:** (boolean) `true` on success, `false` if the seek failed. Use `tell` to read the new position.
- **Errors:** raises on an unknown `whence`, a negative `offset` with `"set"`, and a resulting position that is before the start of the file or beyond 2^31-1. A rejected seek leaves the position alone. A position past the end of the file is left to the SD driver: on the device a read-only handle stops at the end of the file (`tell` reports the file length), while a writable handle grows the file at seek time (the gap holds undefined data) and `tell` reports the requested position. The simulator differs: it does not clamp or extend on a seek, only on a later write, so keep seeks within the file if the result matters.

```lua
picocalc.fs.seek(f, 0)            -- Seek to beginning
picocalc.fs.seek(f, 0, "end")     -- Seek to the end...
local length = picocalc.fs.tell(f)  -- ...so tell() is the file length
picocalc.fs.seek(f, -4, "cur")    -- Back up four bytes
```

---

#### `picocalc.fs.tell(file)`
Returns the current byte position within an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
- **Returns:** (number) Current byte offset

```lua
local pos = picocalc.fs.tell(f)
```

---

#### `file:fsize()`
The total size of an **open** file in bytes, without moving the read position.

- **Returns:** (number or nil, string) Size in bytes, or `nil` plus an error message if it could not be read. Callable as `f:fsize()` or `picocalc.fs.fsize(f)`.
- **Errors:** raises if the handle is closed

`seek(f, 0, "end")` followed by `tell(f)` gives the same number but leaves the
handle at end of file, so it changes where the next `read` starts. This does not:

```lua
local f = picocalc.fs.open(picocalc.fs.appPath("level.dat"), "rb")
local total = f:fsize()      -- or picocalc.fs.fsize(f); position unchanged
local chunk = f:read(16)     -- still the first 16 bytes
```

---

#### `picocalc.fs.appPath(name)`
Returns the path `/data/<appname>/<name>`, automatically creating the app's data directory if it does not exist. This is the recommended way to access per-app persistent storage.

- **Parameters:**
  - `name` (string): Filename within the app's data directory
- **Returns:** (string) Full path (e.g., `"/data/myapp/save.json"`)

```lua
local save_path = picocalc.fs.appPath("save.json")
local f = picocalc.fs.open(save_path, "w")
picocalc.fs.write(f, '{"score": 100}')
picocalc.fs.close(f)
```

---

#### `picocalc.fs.browse([startDir])`
Opens a file-browser overlay panel. The user can navigate directories and select a file; Esc goes up a directory and cancels at the top, and the system menu key cancels from any directory. It is system UI: it draws in the built-in 6x8 font and your font is restored.

- **Parameters:**
  - `startDir` (string, optional): Starting directory. Defaults to the app's `/data/<appname>/` directory.
- **Returns:** (string or nil) Selected file path, or `nil` if cancelled

The top is `/data/<APP_ID>` for a start inside it; any other start directory is its own top, so the user cannot walk above where you put them. An app with `"root-filesystem"` can pass `"/"` to offer the whole card.

```lua
local selected = picocalc.fs.browse("/apps")
if selected then
    print("Selected: " .. selected)
end
```

---

#### `picocalc.fs.copy(src, dst)`
Copy a file. Subject to filesystem sandbox.

- **Parameters:**
  - `src` (string): Source path (needs read access)
  - `dst` (string): Destination path (needs write access)
  - `progress_fn` (function, optional): `function(done, total)`, called after each 4 KB copied. Errors raised inside it are ignored. An exit requested from it (or from the system menu it lets the user open) stops the copy.
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.copy("/data/myapp/save.json", "/data/myapp/save_backup.json")
if not ok then print("Copy failed: " .. err) end
```

---

#### `picocalc.fs.delete(path)`
Delete a file. Subject to filesystem sandbox.

- **Parameters:**
  - `path` (string): Path to delete
- **Returns:** (boolean) `true` on success; `(false, string)` on failure (`"permission denied"`, `"delete failed"`: missing, a directory with files in it, or a file still open; `"cannot delete the volume root"`)

```lua
local ok, err = picocalc.fs.delete("/data/myapp/old_save.json")
if not ok then print("Delete failed: " .. err) end
```

---

#### `picocalc.fs.deleteRecursive(path)`
Delete a directory and all its contents, or a single file. The volume root is refused, for every app.

- **Parameters:**
  - `path` (string): Directory path to delete
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.deleteRecursive("/data/myapp/cache")
if not ok then print("Delete failed: " .. err) end
```

---

#### `picocalc.fs.rename(src, dst)`
Rename or move a file or directory. The parent of `dst` must exist.

**`rename` refuses an existing `dst`**: unlike POSIX, it does not replace it (`false, "rename failed"`). That includes a change of case alone, since names are not case-sensitive. Delete the old file first; see [Saving a file safely](#saving-a-file-safely).

- **Parameters:**
  - `src` (string): Current path
  - `dst` (string): New path
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.rename("/data/myapp/temp.txt", "/data/myapp/final.txt")
if not ok then print("Rename failed: " .. err) end
```

---

#### `picocalc.fs.stat(path)`
Get file or directory information. Subject to sandbox.

- **Parameters:**
  - `path` (string): File or directory path
- **Returns:** (table) `{size = number, is_dir = boolean}`, or `(nil, string)` on failure

```lua
local info, err = picocalc.fs.stat("/data/myapp/save.json")
if info then
    print("Size: " .. info.size .. ", is_dir: " .. tostring(info.is_dir))
end
```

---

#### `picocalc.fs.diskInfo()`
Get SD card disk space information.

- **Parameters:** None
- **Returns:** (table) `{free = number, total = number}` (values in KB), or `(nil, string)` on failure

```lua
local info, err = picocalc.fs.diskInfo()
if info then
    print("Free: " .. info.free .. " KB / Total: " .. info.total .. " KB")
end
```

---

#### `picocalc.fs.glob(path, pattern)`
List the entries of a directory whose names match a glob pattern. `*` matches any sequence and `?` any one character, ignoring case. Subdirectories match too; test `is_dir` if you want only files. It does not look inside them.

- **Parameters:**
  - `path` (string): Directory to search
  - `pattern` (string): Glob pattern (e.g. `"*.lua"`)
- **Returns:** (table) Array of matching filenames

```lua
local lua_files = picocalc.fs.glob("/apps/myapp", "*.lua")
for _, name in ipairs(lua_files) do
    print(name)
end
```

---

#### `picocalc.fs.ensureReady()`
Ensure the SD card is mounted and ready.

- **Parameters:** None
- **Returns:** (boolean) `true` if SD card is ready

```lua
if picocalc.fs.ensureReady() then
    -- Safe to perform file operations
end
```

---

#### `picocalc.fs.setSlowMode(enabled)`
Enable or disable slow SD card mode. Slow mode reduces SPI clock speed for compatibility with some SD cards.

- **Parameters:**
  - `enabled` (boolean): `true` to enable slow mode, `false` to disable
- **Returns:** None

```lua
picocalc.fs.setSlowMode(true)  -- Use slower SPI clock for compatibility
```

---

### Saving a file safely

Writing over a file in place loses it if the power goes or the card fills part way. Write a temporary file, check it, and only then replace the old one. `rename` will not replace an existing file on the device, so delete the old one first:

```lua
local function save(name, text)
    local path = picocalc.fs.appPath(name)
    local tmp = path .. ".tmp"
    local f, err = picocalc.fs.open(tmp, "w")
    if not f then return false, err end
    local n = f:write(text)
    f:close()
    -- A full card is a short count, not an error, and close reports nothing.
    if n ~= #text or picocalc.fs.size(tmp) ~= #text then
        picocalc.fs.delete(tmp)
        return false, "write failed"
    end
    picocalc.fs.delete(path)  -- rename refuses an existing target
    return picocalc.fs.rename(tmp, path)
end

local ok, err = save("save.json", '{"score": 100}')
if not ok then print("Save failed: " .. err) end
```

If the power goes between the `delete` and the `rename`, only the `.tmp` is left, and it is complete: when loading, use it if the file itself is missing.

---

### Long calls

The device resets if the system goes 10 seconds without feeding its watchdog. The calls here keep feeding it, whatever the size of the file, but your app does not run while one is in progress, so the system menu and Exit App wait until it returns. At the card's roughly 300-480 KB/s (about 100 KB/s under slow mode), reading, writing or copying a few MB in one call takes seconds. Read large files in pieces, or give `copy` a progress function, to stay responsive.

---

### Differences from the native C API

The native `g_api.fs` table (`picocalc_fs_t`) and this Lua table overlap but are not identical. Porting a native app, or reading the C header:

| Native C (`api->fs->...`) | Lua | Note |
|---|---|---|
| `fsize(file)` | `picocalc.fs.fsize(f)` / `f:fsize()`, or `picocalc.fs.size(path)` for a path | Same. `seek(f, 0, "end")` + `tell(f)` also works but moves the position |
| `isDir(path)` | `picocalc.fs.isDir(path)` | Same. `stat(p).is_dir` also works but needs `st and st.is_dir`, since `stat` returns `nil` for a missing path |
| `seek(file, offset)` (absolute, `bool`) | `picocalc.fs.seek(f, offset)` | Same default; Lua adds `whence` (`"set"`, `"cur"`, `"end"`); C has absolute seeks only |
| `tell(file)` | `picocalc.fs.tell(f)` | Same |
| `read(file, buf, len)` (fills a buffer, returns a count) | `picocalc.fs.read(f, len)` (returns a string, `nil` at end of file) | |
| `open(path, mode)` (`NULL` on error) | `picocalc.fs.open(path [, mode])` (`nil, err`) | Lua handles are objects with methods and close on garbage collection |

Lua-only additions: `readFile`, `appPath`, `copy`, `deleteRecursive`, `stat`, `diskInfo`, `glob`, `ensureReady` and `setSlowMode`. Names differ for two functions: C `deleteFile` is `picocalc.fs.delete`, and C `renameFile` is `picocalc.fs.rename`; C `listDir` takes a callback where Lua returns a table.
