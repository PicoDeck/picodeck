-- Filesystem test fixture (picotest kit). Runs with root-filesystem, so it
-- covers the fs API itself; sandbox enforcement is sandbox_test's job.
-- Idempotent: it removes its own files first, so it passes on every run
-- against the same SD card.

local pc = picocalc
local fs = pc.fs
local T = pc.sys.loadlib("picotest")

local FILES = {
    "test_write.txt", "test_readfile.txt", "test_exists.txt", "test_size.txt",
    "test_seek.txt", "test_delete.txt", "test_rename_src.txt",
    "test_rename_dst.txt", "test_large.bin", "test_subdir/nested.txt",
    "test_subdir", "test_rn_src.txt", "test_rn_dst.txt", "test_save.json",
    "test_save.json.tmp", "test_modes.txt", "test_a_file", "test_copy_src.bin",
    "test_copy_dst.bin", "test_glob_A.TXT", "test_glob_b.txt", "test_stat_dir",
}
for _, name in ipairs(FILES) do
    local p = fs.appPath(name)
    if fs.exists(p) then fs.delete(p) end
end

local function write(path, data)
    local f = T.ok(fs.open(path, "w"), "open for write: " .. path)
    fs.write(f, data)
    fs.close(f)
end

pc.display.clear(pc.display.BLACK)
pc.display.drawText(10, 10, "Running FS tests...", pc.display.WHITE)
pc.display.flush()

T.case("write_read", function()
    local path = fs.appPath("test_write.txt")
    local data = "Hello, PicoDeck filesystem!"
    write(path, data)
    local f = T.ok(fs.open(path, "r"), "open for read")
    local got = fs.read(f, #data + 10)
    fs.close(f)
    T.eq(got, data)
end)

T.case("readFile", function()
    local path = fs.appPath("test_readfile.txt")
    write(path, "readFile test data 12345")
    T.eq(fs.readFile(path), "readFile test data 12345")
end)

T.case("exists", function()
    local path = fs.appPath("test_exists.txt")
    T.eq(fs.exists(path), false, "exists before write")
    write(path, "x")
    T.eq(fs.exists(path), true, "exists after write")
end)

T.case("size", function()
    local path = fs.appPath("test_size.txt")
    write(path, string.rep("A", 100))
    T.eq(fs.size(path), 100)
end)

T.case("seek_tell", function()
    local path = fs.appPath("test_seek.txt")
    write(path, "ABCDEFGHIJ")
    local f = T.ok(fs.open(path, "r"), "open for read")
    fs.seek(f, 5)
    local pos = fs.tell(f)
    local got = fs.read(f, 5)
    fs.close(f)
    T.eq(pos, 5, "tell")
    T.eq(got, "FGHIJ", "read after seek")
end)

T.case("seek_whence", function()
    local path = fs.appPath("test_seek.txt")
    write(path, "ABCDEFGHIJ")
    local f = T.ok(fs.open(path, "r"), "open for read")

    -- "end": offset 0 lands on the length; it can be told and read from.
    T.eq(fs.seek(f, 0, "end"), true, "seek end")
    T.eq(fs.tell(f), 10, "tell after seek end = file length")
    T.eq(fs.read(f, 1), nil, "read at end")
    -- Negative offsets from the end.
    T.eq(fs.seek(f, -3, "end"), true, "seek end-3")
    T.eq(fs.read(f, 3), "HIJ", "read last three")

    -- "cur" is relative to the current position, both ways.
    fs.seek(f, 2)
    T.eq(fs.seek(f, 3, "cur"), true, "seek cur+3")
    T.eq(fs.tell(f), 5, "tell after cur+3")
    T.eq(fs.seek(f, -4, "cur"), true, "seek cur-4")
    T.eq(fs.tell(f), 1, "tell after cur-4")
    T.eq(fs.seek(f, 0, "cur"), true, "seek cur+0")
    T.eq(fs.tell(f), 1, "cur+0 does not move")

    -- Explicit "set" equals the default; the method form takes whence too.
    T.eq(fs.seek(f, 4, "set"), true)
    T.eq(fs.tell(f), 4, "explicit set")
    T.eq(f:seek(-2, "end"), true, "method seek end")
    T.eq(f:tell(), 8, "method tell")

    -- Default (no whence) is unchanged: absolute, negatives rejected.
    T.eq(fs.seek(f, 5), true)
    T.eq(fs.tell(f), 5, "default whence is set")
    T.raises(function() fs.seek(f, -1) end, "negative offset")
    T.raises(function() fs.seek(f, -1, "set") end, "negative offset")
    T.eq(fs.tell(f), 5, "failed set left the position alone")

    -- Bad whence, and positions before the start or out of range, raise.
    T.raises(function() fs.seek(f, 0, "bogus") end, "invalid option")
    T.raises(function() fs.seek(f, 0, 1) end, "invalid option")
    T.raises(function() fs.seek(f, -11, "end") end, "before start")
    T.raises(function() fs.seek(f, -6, "cur") end, "before start")
    T.raises(function() fs.seek(f, 2147483647, "end") end, "out of range")
    T.eq(fs.tell(f), 5, "rejected seeks left the position alone")
    fs.close(f)
end)

T.case("mkdir", function()
    local dir = fs.appPath("test_subdir")
    T.ok(fs.mkdir(dir), "mkdir failed")
    write(dir .. "/nested.txt", "nested content")
    T.eq(fs.readFile(dir .. "/nested.txt"), "nested content")
end)

T.case("delete", function()
    local path = fs.appPath("test_delete.txt")
    write(path, "delete me")
    T.ok(fs.exists(path), "not created")
    fs.delete(path)
    T.eq(fs.exists(path), false, "still exists after delete")
end)

T.case("rename", function()
    local src = fs.appPath("test_rename_src.txt")
    local dst = fs.appPath("test_rename_dst.txt")
    write(src, "rename data")
    T.ok(fs.rename(src, dst), "rename failed")
    T.eq(fs.exists(src), false, "source still exists")
    T.eq(fs.readFile(dst), "rename data")
end)

T.case("listDir", function()
    local entries = T.ok(fs.listDir(fs.appPath("")), "listDir returned nil")
    local names = {}
    for _, e in ipairs(entries) do names[e.name] = e.is_dir end
    T.eq(names["test_write.txt"], false, "test_write.txt listed as a file")
    T.eq(names["test_subdir"], true, "test_subdir listed as a dir")
end)

T.case("large_write", function()
    local path = fs.appPath("test_large.bin")
    local chunk = string.rep("X", 512)
    local f = T.ok(fs.open(path, "w"), "open")
    for _ = 1, 64 do fs.write(f, chunk) end   -- 32 KB, multi-block
    fs.close(f)
    T.eq(fs.size(path), 32768)
end)

T.case("large_read", function()
    local f = T.ok(fs.open(fs.appPath("test_large.bin"), "r"), "open")
    local data = fs.read(f, 32768)
    fs.close(f)
    T.eq(data and #data, 32768, "length")
    T.eq(data, string.rep("X", 32768), "content")
end)

T.case("diskInfo", function()
    local info = T.ok(fs.diskInfo(), "diskInfo returned nil")
    T.eq(type(info.free), "number", "free")
    T.eq(type(info.total), "number", "total")
    T.ok(info.total > 0 and info.free >= 0, "invalid values")
end)

-- ── Device semantics the simulator models (FatFS) ────────────────────────────

T.case("rename_onto_existing_refused", function()
    local src, dst = fs.appPath("test_rn_src.txt"), fs.appPath("test_rn_dst.txt")
    write(src, "new")
    write(dst, "old")
    local ok, err = fs.rename(src, dst)
    T.eq(ok, false, "rename onto an existing file")
    T.eq(err, "rename failed", "reason")
    T.eq(fs.readFile(src), "new", "source untouched")
    T.eq(fs.readFile(dst), "old", "target untouched")
end)

-- The save pattern the docs give: write a temp file, delete, rename.
T.case("save_pattern_temp_then_rename", function()
    local path = fs.appPath("test_save.json")
    for _, text in ipairs({ "first", "second" }) do
        local tmp = path .. ".tmp"
        local f = T.ok(fs.open(tmp, "w"), "open tmp")
        T.eq(f:write(text), #text, "write")
        f:close()
        if fs.exists(path) then T.ok(fs.delete(path), "delete old") end
        T.ok(fs.rename(tmp, path), "rename")
        T.eq(fs.readFile(path), text, "saved")
    end
    T.eq(fs.exists(path .. ".tmp"), false, "tmp gone")
end)

T.case("mode_r_plus_is_read_only", function()
    local path = fs.appPath("test_modes.txt")
    write(path, "abc")
    local f = T.ok(fs.open(path, "r+"), "open r+")
    T.eq(f:write("Q"), -1, "write on r+")
    T.eq(f:read(3), "abc", "read on r+")
    f:close()
    T.eq(fs.readFile(path), "abc", "unchanged")
    T.eq(fs.open(fs.appPath("test_absent"), "r+"), nil, "r+ does not create")
end)

T.case("mode_w_plus_is_write_only", function()
    local path = fs.appPath("test_modes.txt")
    write(path, "old contents")
    local f = T.ok(fs.open(path, "w+"), "open w+")
    T.eq(f:write("hi"), 2, "write")
    T.ok(f:seek(0), "seek")
    T.eq(f:read(2), nil, "read on w+")
    f:close()
    T.eq(fs.readFile(path), "hi", "truncated and written")
end)

T.case("mode_rw_reads_and_writes", function()
    local path = fs.appPath("test_modes.txt")
    write(path, "abcdef")
    local f = T.ok(fs.open(path, "rw"), "open rw")
    T.eq(f:read(2), "ab", "read")
    T.eq(f:write("XY"), 2, "write after read")
    T.eq(f:read(2), "ef", "read after write")
    f:close()
    T.eq(fs.readFile(path), "abXYef", "no truncation")
    T.ok(fs.delete(path), "delete")
    f = T.ok(fs.open(path, "rw"), "rw creates a missing file")
    f:close()
    T.eq(fs.size(path), 0, "created empty")
end)

T.case("mode_a_appends", function()
    local path = fs.appPath("test_modes.txt")
    write(path, "abc")
    local f = T.ok(fs.open(path, "a"), "open a")
    T.eq(f:write("d"), 1, "write")
    f:close()
    T.eq(fs.readFile(path), "abcd", "appended")
    T.ok(fs.delete(path), "delete")
    f = T.ok(fs.open(path, "ab"), "a creates a missing file")
    T.eq(f:read(1), nil, "read on a")
    f:close()
end)

T.case("write_on_read_handle", function()
    local path = fs.appPath("test_modes.txt")
    write(path, "abc")
    local f = T.ok(fs.open(path, "r"), "open")
    T.eq(f:write("Q"), -1, "write on a read handle")
    f:close()
    T.eq(fs.readFile(path), "abc", "unchanged")
end)

T.case("mkdir_details", function()
    T.eq(fs.mkdir(fs.appPath("test_no_parent/child")), false, "a missing parent")
    local dir = fs.appPath("test_stat_dir")
    T.ok(fs.mkdir(dir), "new directory")
    T.ok(fs.mkdir(dir), "an existing directory is fine")
    local file = fs.appPath("test_a_file")
    write(file, "x")
    T.eq(fs.mkdir(file), false, "a file of that name")
    T.eq(fs.isDir(file), false, "still a file")
end)

T.case("volume_root_cannot_be_deleted", function()
    for _, root in ipairs({ "/", "//", "/.", "/./" }) do
        local ok, err = fs.deleteRecursive(root)
        T.eq(ok, false, "deleteRecursive " .. root)
        T.ok(err, "reason")
        T.eq(fs.delete(root), false, "delete " .. root)
    end
    T.ok(fs.isDir("/apps"), "/apps survived")
    T.ok(fs.isDir("/data"), "/data survived")
end)

T.case("stat_fields", function()
    local file = fs.appPath("test_a_file")
    write(file, "12345")
    local st = T.ok(fs.stat(file), "stat")
    T.eq(st.size, 5, "size")
    T.eq(st.is_dir, false, "is_dir")
    T.ok(st.year and st.year >= 1980, "year")
    T.ok(st.month and st.day and st.hour and st.min and st.sec, "time fields")
    T.ok(fs.mkdir(fs.appPath("test_stat_dir")), "mkdir")
    local d = T.ok(fs.stat(fs.appPath("test_stat_dir")), "stat dir")
    T.eq(d.is_dir, true, "dir is_dir")
    T.eq(d.size, 0, "a directory's size")
    for _, e in ipairs(fs.listDir(fs.appPath(""))) do
        if e.is_dir then T.eq(e.size, 0, "listDir size of " .. e.name) end
    end
    T.eq(fs.size(fs.appPath("test_stat_dir")), 0, "fs.size of a directory")
end)

T.case("glob_returns_entries", function()
    write(fs.appPath("test_glob_A.TXT"), "a")
    write(fs.appPath("test_glob_b.txt"), "bb")
    local found = {}
    for _, e in ipairs(fs.glob(fs.appPath(""), "test_glob_*.txt")) do
        found[e.name] = e
    end
    T.ok(found["test_glob_A.TXT"], "case-insensitive match")
    T.eq(found["test_glob_b.txt"] and found["test_glob_b.txt"].size, 2, "size")
    T.eq(found["test_glob_b.txt"].is_dir, false, "is_dir")
end)

T.done()
