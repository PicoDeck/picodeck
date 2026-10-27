#!/usr/bin/env lua
-- Lua App Syntax Checker for PicoDeck
-- Tests all Lua apps for syntax errors and common issues before deployment

local colors = {
    reset = "\27[0m",
    red = "\27[31m",
    green = "\27[32m",
    yellow = "\27[33m",
    blue = "\27[34m",
    cyan = "\27[36m",
}

local function color(c, text)
    return colors[c] .. text .. colors.reset
end

local function log_info(msg)
    print(color("cyan", "[INFO] ") .. msg)
end

local function log_success(msg)
    print(color("green", "[✓] ") .. msg)
end

local function log_warning(msg)
    print(color("yellow", "[⚠] ") .. msg)
end

local function log_error(msg)
    print(color("red", "[✗] ") .. msg)
end

-- Mock PicoDeck API to allow code to parse
local function create_mock_api()
    local mock_display = {
        BLACK = 0x0000, WHITE = 0xFFFF, RED = 0xF800, GREEN = 0x07E0,
        BLUE = 0x001F, YELLOW = 0xFFE0, CYAN = 0x07FF, GRAY = 0x8410,
        clear = function() end,
        setPixel = function() end,
        fillRect = function() end,
        drawRect = function() end,
        drawLine = function() end,
        drawText = function() return 0 end,
        flush = function() end,
        getWidth = function() return 320 end,
        getHeight = function() return 320 end,
        setBrightness = function() end,
        textWidth = function() return 0 end,
        rgb = function(r, g, b) return 0 end,
    }
    
    local mock_input = {
        BTN_UP = 1, BTN_DOWN = 2, BTN_LEFT = 4, BTN_RIGHT = 8,
        BTN_ENTER = 16, BTN_ESC = 32, BTN_MENU = 64,
        BTN_F1 = 128, BTN_F2 = 256, BTN_F3 = 512, BTN_F4 = 1024,
        BTN_F5 = 2048, BTN_F6 = 4096, BTN_F7 = 8192, BTN_F8 = 16384, BTN_F9 = 32768,
        BTN_BACKSPACE = 65536, BTN_TAB = 131072, BTN_DEL = 262144,
        BTN_SHIFT = 524288, BTN_CTRL = 1048576, BTN_ALT = 2097152, BTN_FN = 4194304,
        update = function() end,
        getButtons = function() return 0 end,
        getButtonsPressed = function() return 0 end,
        getButtonsReleased = function() return 0 end,
        getChar = function() return nil end,
        getRawKey = function() return 0 end,
    }
    
    local mock_sys = {
        getTimeMs = function() return 0 end,
        sleep = function() end,
        getBattery = function() return 50 end,
        isUSBPowered = function() return false end,
        log = function() end,
        exit = function() end,
        reboot = function() end,
        addMenuItem = function() end,
        addMenuOption = function() end,
        clearMenuItems = function() end,
    }
    
    local mock_fs = {
        open = function() return nil end,
        read = function() return nil end,
        write = function() return 0 end,
        close = function() end,
        exists = function() return false end,
        readFile = function() return nil end,
        size = function() return 0 end,
        listDir = function() return {} end,
    }
    
    local mock_wifi = {
        STATUS_DISCONNECTED = 0, STATUS_CONNECTING = 1,
        STATUS_CONNECTED = 2, STATUS_FAILED = 3,
        isAvailable = function() return false end,
        connect = function() end,
        disconnect = function() end,
        getStatus = function() return 0 end,
        getIP = function() return nil end,
        getSSID = function() return nil end,
    }
    
    local mock_config = {
        get = function() return nil end,
        set = function() end,
        save = function() return true end,
        load = function() return true end,
    }
    
    local mock_perf = {
        beginFrame = function() end,
        endFrame = function() end,
        getFPS = function() return 60 end,
        getFrameTime = function() return 16 end,
        drawFPS = function() end,
    }
    
    local mock_ui = {
        drawHeader = function() end,
        drawFooter = function() end,
    }

    return {
        display = mock_display,
        input = mock_input,
        sys = mock_sys,
        fs = mock_fs,
        wifi = mock_wifi,
        config = mock_config,
        perf = mock_perf,
        ui = mock_ui,
    }
end

-- Check syntax of a Lua file
local function check_lua_file(filepath)
    local file = io.open(filepath, "r")
    if not file then
        return false, "Cannot open file"
    end
    
    local content = file:read("*all")
    file:close()
    
    -- Try to load the chunk
    local chunk, err = load(content, "@" .. filepath, "t", {
        picocalc = create_mock_api(),
        pc = create_mock_api(),
        APP_DIR = "/apps/test",
        APP_NAME = "Test App",
    })
    
    if not chunk then
        return false, err
    end
    
    return true, nil
end

-- Recursively find all .lua files in a directory. Inside a git checkout,
-- only files git would track (committed or untracked-but-not-ignored): the
-- gitignored vendor trees (e.g. apps/tic-80's bundled Lua test suite, which
-- is deliberately invalid Lua) are not PicoDeck apps.
local function find_lua_files(dir)
    local files = {}
    local handle = io.popen("git ls-files --cached --others --exclude-standard -- '"
        .. dir .. "/*.lua' 2>/dev/null")
    if handle then
        for file in handle:lines() do
            table.insert(files, file)
        end
        handle:close()
    end
    if #files > 0 then return files end

    handle = io.popen("find " .. dir .. " -name '*.lua' 2>/dev/null")
    if not handle then return files end
    for file in handle:lines() do
        table.insert(files, file)
    end
    handle:close()
    return files
end

-- Main test runner
local function main()
    print(color("blue", "\n=== PicoDeck Lua App Syntax Checker ===\n"))
    
    local apps_dir = "apps"
    if not io.open(apps_dir, "r") then
        log_error("Cannot find apps directory")
        os.exit(1)
    end
    
    local lua_files = find_lua_files(apps_dir)
    
    if #lua_files == 0 then
        log_warning("No Lua files found in " .. apps_dir)
        return
    end
    
    log_info("Found " .. #lua_files .. " Lua file(s) to check\n")
    
    local failed = 0
    local passed = 0
    local errors = {}
    
    for _, filepath in ipairs(lua_files) do
        local ok, err = check_lua_file(filepath)
        
        if ok then
            log_success(filepath)
            passed = passed + 1
        else
            log_error(filepath)
            print("  " .. color("red", err))
            failed = failed + 1
            table.insert(errors, {file = filepath, error = err})
        end
    end
    
    print("\n" .. color("blue", "=== Summary ==="))
    print(color("green", "Passed: " .. passed))
    if failed > 0 then
        print(color("red", "Failed: " .. failed))
    end
    
    if failed > 0 then
        print("\n" .. color("yellow", "Fix these errors before deploying to device!"))
        os.exit(1)
    else
        print("\n" .. color("green", "All checks passed! ✓"))
        os.exit(0)
    end
end

main()
