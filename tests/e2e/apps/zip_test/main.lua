-- E2E fixture for the hardened ZIP engine and archive handles.
-- The test generates hostile.zip / caps.zip / manyfiles.zip / truncated.zip
-- into this app's directory before launching; every probe logs a "ZE " marker.

local log = picocalc.sys.log
local zip = picocalc.zip
local fs  = picocalc.fs
local base = APP_DIR .. "/"
local out  = "/data/" .. APP_ID .. "/out"

-- 1 ── extractAll of a hostile archive: succeeds, skips unsafe names.
local ok, err = zip.extract(base .. "hostile.zip", out)
log("ZE HOSTILE ok=" .. tostring(ok) .. " err=" .. tostring(err))
log("ZE HOSTILE_GOOD " .. tostring(fs.readFile(out .. "/good.txt")))
log("ZE HOSTILE_NESTED " .. tostring(fs.readFile(out .. "/dir/nested.txt")))

-- 2 ── handle API basics on the same archive.
local ar = zip.open(base .. "hostile.zip")
log("ZE OPEN " .. tostring(ar ~= nil))
if ar then
    log("ZE EXISTS " .. tostring(ar:exists("good.txt"))
        .. " " .. tostring(ar:exists("missing.txt")))
    log("ZE READ " .. tostring(ar:read("good.txt")))
    local _, merr = ar:read("missing.txt")
    log("ZE READMISS " .. tostring(merr))
    ar:close()
end

-- 3 ── single in-memory read cap (entry bigger than ZIP_MAX_READ_MEM).
local ar2 = zip.open(base .. "caps.zip")
if ar2 then
    local d, e = ar2:read("big.bin")
    log("ZE BIGREAD data=" .. tostring(d ~= nil) .. " err=" .. tostring(e))
    -- ...but streaming the same entry to disk is fine.
    local xok = ar2:extract("big.bin", out .. "/big.bin")
    log("ZE BIGX ok=" .. tostring(xok)
        .. " size=" .. tostring(fs.size(out .. "/big.bin")))
    ar2:close()
end

-- 4 ── entry-count cap: extract must refuse fast, before writing anything.
local ok3, err3 = zip.extract(base .. "manyfiles.zip", out .. "2")
log("ZE MANY ok=" .. tostring(ok3) .. " err=" .. tostring(err3))

-- 5 ── truncated archive: open fails cleanly.
local ar3, e3 = zip.open(base .. "truncated.zip")
log("ZE TRUNC nil=" .. tostring(ar3 == nil) .. " err=" .. tostring(e3))

-- 6 ── open budget: 4 concurrent, 5th refused, budget released on close.
local hs = {}
for i = 1, 4 do hs[i] = zip.open(base .. "hostile.zip") end
local fifth = zip.open(base .. "hostile.zip")
log("ZE FIFTH nil=" .. tostring(fifth == nil))
for i = 1, 4 do if hs[i] then hs[i]:close() end end

-- 7 ── closed-handle use raises; double close is a no-op.
local a = zip.open(base .. "hostile.zip")
a:close()
a:close()
local pok = pcall(function() return a:list() end)
log("ZE CLOSED_RAISES " .. tostring(not pok))

-- 8 ── a leaked (unreferenced) handle is reclaimed by GC, freeing budget.
do local _ = zip.open(base .. "hostile.zip") end
collectgarbage("collect")
local gs = {}
for i = 1, 4 do gs[i] = zip.open(base .. "hostile.zip") end
log("ZE GC4 " .. tostring(gs[4] ~= nil))
for i = 1, 4 do if gs[i] then gs[i]:close() end end

-- 9 ── a destination that cannot be opened: named, not "write failed".
fs.mkdir(out .. "3")
fs.mkdir(out .. "3/good.txt")
local ok9, err9 = zip.extract(base .. "hostile.zip", out .. "3")
log("ZE BLOCKED ok=" .. tostring(ok9) .. " err=" .. tostring(err9))
local ar9 = zip.open(base .. "hostile.zip")
local ok10, err10 = ar9:extract("good.txt", out .. "3/good.txt")
log("ZE BLOCKED1 ok=" .. tostring(ok10) .. " err=" .. tostring(err10))
ar9:close()

log("ZE DONE")
