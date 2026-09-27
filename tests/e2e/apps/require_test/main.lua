local T = picocalc.sys.loadlib("picotest")

T.case("loads_app_module_with_name_and_path", function()
    local a = require("mod_a")
    T.eq(a.value, 42)
    T.eq(a.name, "mod_a")
    T.eq(a.path, APP_DIR .. "/mod_a.lua")
end)

T.case("dotted_name_maps_to_subdirectory", function()
    T.eq(require("sub.mod_b").value, "b")
end)

T.case("caches_the_result", function()
    local c1 = require("counter")
    local c2 = require("counter")
    T.ok(c1 == c2)
    T.eq(LOAD_COUNT, 1)
end)

T.case("nil_result_is_cached_as_true", function()
    T.eq(require("nilmod"), true)
    T.eq(NILMOD_RAN, true)
end)

T.case("false_result_is_kept", function()
    T.eq(require("falsemod"), false)
    T.eq(require("falsemod"), false)
end)

T.case("falls_back_to_system_lib", function()
    local lib = require("picotest")
    T.eq(type(lib), "table")
    T.eq(type(lib.case), "function")
end)

T.case("cycle_raises", function()
    T.raises(function() require("cyc_a") end, "circular require of 'cyc_a'")
end)

T.case("failed_module_can_be_retried", function()
    T.raises(function() require("errmod") end, "first load fails")
    T.eq(require("errmod"), "second load works")
end)

T.case("not_found_lists_both_paths", function()
    local err = T.raises(function() require("nope") end)
    T.ok(err:find(APP_DIR .. "/nope.lua", 1, true), err)
    T.ok(err:find("/system/lib/nope.lua", 1, true), err)
end)

T.case("hostile_names_rejected", function()
    for _, n in ipairs({ "", "..", "a..b", ".a", "a.", "a/b", "../x", "x/../../y", ("a"):rep(200) }) do
        T.raises(function() require(n) end, "invalid module name")
    end
    T.raises(function() require(42) end)
end)

T.done()
