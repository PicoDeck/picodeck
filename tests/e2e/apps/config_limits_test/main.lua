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
