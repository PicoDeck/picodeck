---
title: "API Filesystem"
---

Filesystem access to the SD card (FAT32).

## picocalc.fs

### Functions

#### `picocalc.fs.open(path [, mode])`
Opens a file on the SD card.

- **Parameters:**
  - `path` (string): Absolute file path (e.g., `"/apps/hello/data.txt"`)
  - `mode` (string, optional): File mode (`"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, etc.). Defaults to `"r"`. A mode containing `r` but no `w` or `a` (including `"r+"`) opens the file read-only.
- **Returns:** a file handle (userdata), or `nil, err` (`"permission denied"`, `"cannot open file"`, `"too many open files"`)

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
- **Returns:** (string or nil) Data read, or `nil` on error

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
- **Returns:** (number) Number of bytes written

```lua
local n = picocalc.fs.write(f, "content")
```

---

#### `picocalc.fs.close(file)`
Closes an open file.

- **Parameters:**
  - `file` (userdata): File handle from `open()`
- **Returns:** None

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
- **Returns:** (table) Array of entries, where each entry is a table with:
  - `name` (string): File or directory name
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
Creates a directory at the specified path.

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
local f = picocalc.fs.open("/data/level.dat", "rb")
local total = f:fsize()      -- position unchanged
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
Opens a file-browser overlay panel. The user can navigate directories and select a file; Esc goes up a directory (and cancels at the top), and the system menu key cancels from any directory.

- **Parameters:**
  - `startDir` (string, optional): Starting directory. Defaults to the app's `/data/<appname>/` directory.
- **Returns:** (string or nil) Selected file path, or `nil` if cancelled

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
  - `src` (string): Source path
  - `dst` (string): Destination path
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.copy("/data/myapp/save.json", "/data/myapp/save_backup.json")
if not ok then print("Copy failed: " .. err) end
```

---

#### `picocalc.fs.delete(path)`
Delete a file. Subject to filesystem sandbox.

- **Parameters:**
  - `path` (string): File path to delete
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.delete("/data/myapp/old_save.json")
if not ok then print("Delete failed: " .. err) end
```

---

#### `picocalc.fs.deleteRecursive(path)`
Delete a directory and all its contents. Subject to filesystem sandbox.

- **Parameters:**
  - `path` (string): Directory path to delete
- **Returns:** (boolean) `true` on success; `(false, string)` on failure

```lua
local ok, err = picocalc.fs.deleteRecursive("/data/myapp/cache")
if not ok then print("Delete failed: " .. err) end
```

---

#### `picocalc.fs.rename(src, dst)`
Rename or move a file. Both paths subject to sandbox.

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
List files in a directory matching a glob pattern. Subject to sandbox.

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
