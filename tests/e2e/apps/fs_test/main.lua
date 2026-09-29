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
    "test_subdir",
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

T.done()
