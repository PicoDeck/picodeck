-- Config store limits (audit §3.9), picotest kit. Runs twice in one
-- simulator: the first launch writes, the second (after a restart of the
-- app) reads back from disk. Every case starts from an empty in-memory store.
local pc = picocalc
local fs = pc.fs
local cfg = pc.config
local T = pc.sys.loadlib("picotest")

local PHASE_FILE = fs.appPath("phase1.done")
local second_run = fs.exists(PHASE_FILE)

local function fresh()
    cfg.clear()
end

if not second_run then
    T.case("appconfig_four_keys", function()
        fresh()
        for i = 1, 4 do cfg.set("k" .. i, "v" .. i) end
        for i = 1, 4 do T.eq(cfg.get("k" .. i), "v" .. i, "k" .. i) end
    end)

    T.case("appconfig_five_keys", function()
        fresh()
        for i = 1, 5 do cfg.set("k" .. i, "v" .. i) end
        for i = 1, 5 do T.eq(cfg.get("k" .. i), "v" .. i, "k" .. i) end
    end)

    -- Sets report success or say why not; nothing is stored over a limit.
    T.case("appconfig_32_keys", function()
        fresh()
        for i = 1, 32 do T.eq(cfg.set("k" .. i, "v" .. i), true, "set k" .. i) end
        local ok, why = cfg.set("k33", "v33")
        T.eq(ok, false, "33rd set")
        T.eq(why, "config full")
        T.eq(cfg.get("k33"), nil)
        T.eq(cfg.set("k1", "again"), true, "replacing an existing key still works")
        T.ok(cfg.save(), "save")
        T.ok(cfg.load(), "load")
        T.eq(cfg.get("k1"), "again")
        for i = 2, 32 do T.eq(cfg.get("k" .. i), "v" .. i, "k" .. i) end
        cfg.set("k32", nil)
        T.eq(cfg.set("k33", "v33"), true, "a delete makes room")
    end)

    T.case("appconfig_too_long_is_refused", function()
        fresh()
        local k31 = string.rep("k", 31)
        T.eq(cfg.set(k31, "v"), true, "31-byte key")
        T.eq(cfg.get(k31), "v")
        local ok, why = cfg.set(string.rep("k", 32), "v")
        T.eq(ok, false, "32-byte key")
        T.eq(why, "key too long")
        T.eq(cfg.get(string.rep("k", 32)), nil)
        local v255 = string.rep("v", 255)
        T.eq(cfg.set("val", v255), true, "255-byte value")
        T.eq(cfg.get("val"), v255)
        ok, why = cfg.set("val", string.rep("w", 256))
        T.eq(ok, false, "256-byte value")
        T.eq(why, "value too long")
        T.eq(cfg.get("val"), v255, "the old value stays")
        ok, why = cfg.set("nul", "a\0b")
        T.eq(ok, false, "an embedded NUL would cut the value short")
        T.eq(why, "value contains a NUL byte")
        ok, why = cfg.set("", "v")
        T.eq(ok, false, "empty key")
        T.eq(why, "empty key")
        T.eq(cfg.set("gone", nil), true, "deleting a missing key")
    end)

    -- load + save never deletes what load could not hold.
    T.case("appconfig_keeps_unloaded_keys", function()
        fresh()
        local parts = { '"num":7', '"flag":true', '"list":[1,{"a":"b"}]',
                        '"' .. string.rep("L", 40) .. '":"lk"',
                        '"big":"' .. string.rep("x", 300) .. '"', '"plain":"p"' }
        for i = 1, 40 do parts[#parts + 1] = '"f' .. i .. '":"v' .. i .. '"' end
        local path = fs.appPath("config.json")
        local f = fs.open(path, "w")
        fs.write(f, "{" .. table.concat(parts, ",") .. "}")
        fs.close(f)
        T.ok(cfg.load(), "load")
        T.eq(cfg.get("plain"), "p")
        T.eq(cfg.get("num"), nil, "a number in the file is not a store value")
        T.eq(cfg.get("big"), nil, "never a truncated copy")
        T.eq(cfg.set("plain", "changed"), true, "set")
        T.ok(cfg.save(), "save")
        local saved = pc.json.decode(fs.readFile(path))
        T.eq(saved.plain, "changed")
        T.eq(saved.num, 7)
        T.eq(saved.flag, true)
        T.eq(saved.list[2].a, "b")
        T.eq(saved[string.rep("L", 40)], "lk")
        T.eq(#saved.big, 300)
        for i = 1, 40 do T.eq(saved["f" .. i], "v" .. i, "f" .. i) end
        T.ok(cfg.reset(), "reset")
        T.eq(fs.exists(path), false, "reset deletes the file")
    end)

    T.case("appconfig_200_char_value", function()
        fresh()
        local long = string.rep("abcdefghij", 20)
        cfg.set("long", long)
        T.ok(cfg.save(), "save")
        cfg.load()
        T.eq(cfg.get("long"), long)
    end)

    T.case("appconfig_special_chars_roundtrip", function()
        fresh()
        local v = 'q"uote b\\ack\nnew}brace'
        cfg.set("special", v)
        T.ok(cfg.save(), "save")
        cfg.load()
        T.eq(cfg.get("special"), v)
    end)

    T.case("appconfig_persist_write", function()
        fresh()
        for i = 1, 4 do cfg.set("p" .. i, "persist" .. i) end
        T.ok(cfg.save(), "save")
    end)

    T.case("sysconfig_eleven_keys", function()
        local sc = T.ok(pc.sysconfig, "picocalc.sysconfig missing")
        for i = 1, 11 do sc.set("e2e_s" .. i, "v" .. i) end
        local missing = {}
        for i = 1, 11 do
            if sc.get("e2e_s" .. i) ~= "v" .. i then missing[#missing + 1] = "e2e_s" .. i end
        end
        for i = 1, 11 do sc.set("e2e_s" .. i, nil) end
        T.eq(#missing, 0, "dropped: " .. table.concat(missing, ","))
    end)

    -- The store is a 1 KB pool of pairs: ten 120-char values overflow it.
    T.case("sysconfig_fill_pool", function()
        local sc = T.ok(pc.sysconfig, "picocalc.sysconfig missing")
        local v = string.rep("x", 120)
        for i = 1, 10 do sc.set("e2e_f" .. i, v) end
        local missing = {}
        for i = 1, 10 do
            if sc.get("e2e_f" .. i) ~= v then missing[#missing + 1] = "e2e_f" .. i end
        end
        for i = 1, 10 do sc.set("e2e_f" .. i, nil) end
        T.eq(#missing, 0, "dropped: " .. table.concat(missing, ","))
    end)

    T.case("sysconfig_200_char_value", function()
        local sc = T.ok(pc.sysconfig, "picocalc.sysconfig missing")
        local long = string.rep("abcdefghij", 20)
        sc.set("e2e_long", long)
        local got = sc.get("e2e_long")
        sc.set("e2e_long", nil)
        T.eq(got, long)
    end)

    local f = fs.open(PHASE_FILE, "w")
    fs.write(f, "1")
    fs.close(f)
else
    T.case("appconfig_persist_read", function()
        T.ok(cfg.load(), "load")
        for i = 1, 4 do T.eq(cfg.get("p" .. i), "persist" .. i, "p" .. i) end
    end)
end

T.done()
