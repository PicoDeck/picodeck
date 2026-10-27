-- System Test App for PicoDeck
-- Tests all exposed system functions

local pc = picocalc
local disp = pc.display
local input = pc.input
local sys = pc.sys
local fs = pc.fs
local wifi = pc.wifi

-- Colors
local BG     = disp.BLACK
local FG     = disp.WHITE
local WARN   = disp.rgb(255, 100, 0)
local GOOD   = disp.rgb(0, 255, 100)
local DIM    = disp.GRAY
local TITLE  = disp.CYAN

-- Test results
local tests = {
    battery = {name = "Battery", status = "WAIT", value = ""},
    usb = {name = "USB Power", status = "WAIT", value = ""},
    time = {name = "Time", status = "WAIT", value = ""},
    log = {name = "Logging", status = "WAIT", value = ""},
    fs_read = {name = "FS Read", status = "WAIT", value = ""},
    fs_write = {name = "FS Write", status = "WAIT", value = ""},
    fs_list = {name = "FS List", status = "WAIT", value = ""},
    fs_mkdir = {name = "FS Mkdir", status = "WAIT", value = ""},
    wifi_avail = {name = "WiFi Avail", status = "WAIT", value = ""},
    wifi_status = {name = "WiFi Status", status = "WAIT", value = ""},
    internet = {name = "Internet", status = "WAIT", value = ""},
    qmi_psram = {name = "QMI PSRAM", status = "WAIT", value = ""},
    pio_psram = {name = "PIO PSRAM", status = "WAIT", value = ""},
    xip_cache = {name = "XIP Cache", status = "WAIT", value = ""},
}

local test_order = {
    "battery", "usb", "time", "log",
    "fs_read", "fs_mkdir", "fs_write", "fs_list",
    "wifi_avail", "wifi_status", "internet",
    "qmi_psram", "pio_psram", "xip_cache",
}

-- Scroll state for Tests tab
local tests_scroll = 0

local mode = "tabs"  -- Always use tabbed interface now
local start_time = 0
local data_dir = "/data/" .. APP_ID
local test_file = data_dir .. "/systest.txt"
local button_state = 0

-- Tab demo state
local active_tab = 1
local tab_labels = {"Tests", "Functions", "Input", "Network", "SD Card", "RAM"}
local nav_keys = {prev = input.BTN_LEFT, next = input.BTN_RIGHT}

-- Functions tab state
local func_sel = 1
local func_items = {
    { label = "Hello World Demo",    desc = "Bouncing box, blink text, FPS counter" },
    { label = "Lua error()",         desc = "Calls error() -> /system/error.log" },
    { label = "Nil index",           desc = "nil.foo access -> /system/error.log" },
    { label = "Stack overflow",      desc = "Infinite recursion -> /system/error.log" },
    { label = "HardFault (reboot)",  desc = "CPU fault -> reboot -> /system/crashlog.txt" },
}

-- Hello demo sub-mode state
local hello_active = false
local hello_frame = 0

-- Input test state (from keytest)
local MAX_HISTORY = 16
local history = {}
local last_raw = 0
local last_char = nil

local function push_history(line)
    table.insert(history, 1, line)
    if #history > MAX_HISTORY then table.remove(history) end
end

local BTN_LABELS = {
    { input.BTN_UP,    "UP" },
    { input.BTN_DOWN,  "DOWN" },
    { input.BTN_LEFT,  "LEFT" },
    { input.BTN_RIGHT, "RIGHT" },
    { input.BTN_ENTER, "Enter" },
    { input.BTN_ESC,   "Esc" },
    { input.BTN_MENU,  "Sym" },
    { input.BTN_TAB,   "Tab" },
}

local function btn_names(mask)
    if mask == 0 then return nil end
    local t = {}
    for _, b in ipairs(BTN_LABELS) do
        if mask & b[1] ~= 0 then t[#t + 1] = b[2] end
    end
    return #t > 0 and table.concat(t, "+") or nil
end

-- ── Test Functions ────────────────────────────────────────────────────────────

local function run_battery_test()
    local bat = sys.getBattery()
    if bat >= 0 then
        tests.battery.status = "PASS"
        tests.battery.value = bat .. "%"
    else
        tests.battery.status = "FAIL"
        tests.battery.value = "Unknown"
    end
end

local function run_usb_test()
    local usb = sys.isUSBPowered()
    tests.usb.status = "INFO"
    tests.usb.value = usb and "Yes" or "No"
end

local function run_time_test()
    local t1 = sys.getTimeMs()
    sys.sleep(10)
    local t2 = sys.getTimeMs()
    local diff = t2 - t1
    if diff >= 8 and diff <= 15 then
        tests.time.status = "PASS"
        tests.time.value = diff .. "ms"
    else
        tests.time.status = "FAIL"
        tests.time.value = diff .. "ms (expect ~10)"
    end
end

local function run_log_test()
    sys.log("System test log message")
    tests.log.status = "PASS"
    tests.log.value = "Check USB serial"
end

local function run_fs_read_test()
    -- Try to read our own app.json
    local data = fs.readFile(APP_DIR .. "/app.json")
    if data and #data > 0 then
        tests.fs_read.status = "PASS"
        tests.fs_read.value = #data .. " bytes"
    else
        tests.fs_read.status = "FAIL"
        tests.fs_read.value = "Failed"
    end
end

local function run_fs_mkdir_test()
    -- Create our data directory
    local success = fs.mkdir(data_dir)
    if success then
        tests.fs_mkdir.status = "PASS"
        tests.fs_mkdir.value = APP_ID
    else
        tests.fs_mkdir.status = "FAIL"
        tests.fs_mkdir.value = "Failed"
    end
end

local function run_fs_write_test()
    -- Write a test file to our data directory
    local f = fs.open(test_file, "w")
    if f then
        local written = fs.write(f, "PicoDeck Test: " .. sys.getTimeMs())
        fs.close(f)
        if written > 0 then
            tests.fs_write.status = "PASS"
            tests.fs_write.value = written .. " bytes"
        else
            tests.fs_write.status = "FAIL"
            tests.fs_write.value = "Write failed"
        end
    else
        tests.fs_write.status = "FAIL"
        tests.fs_write.value = "Open failed"
    end
end

local function run_fs_list_test()
    local entries = fs.listDir(APP_DIR)
    if entries and #entries > 0 then
        tests.fs_list.status = "PASS"
        tests.fs_list.value = #entries .. " entries"
    else
        tests.fs_list.status = "FAIL"
        tests.fs_list.value = "No entries"
    end
end

local function run_wifi_avail_test()
    local avail = wifi.isAvailable()
    if avail then
        tests.wifi_avail.status = "PASS"
        tests.wifi_avail.value = "Hardware OK"
    else
        tests.wifi_avail.status = "INFO"
        tests.wifi_avail.value = "Not available"
    end
end

local function run_wifi_status_test()
    if not wifi.isAvailable() then
        tests.wifi_status.status = "INFO"
        tests.wifi_status.value = "N/A"
        return
    end

    local status = wifi.getStatus()
    if status == wifi.STATUS_CONNECTED then
        local ip = wifi.getIP()
        local ssid = wifi.getSSID()
        tests.wifi_status.status = "PASS"
        tests.wifi_status.value = ssid .. " (" .. ip .. ")"
    elseif status == wifi.STATUS_CONNECTING then
        tests.wifi_status.status = "INFO"
        tests.wifi_status.value = "Connecting..."
    elseif status == wifi.STATUS_FAILED then
        tests.wifi_status.status = "WARN"
        tests.wifi_status.value = "Failed"
    else
        tests.wifi_status.status = "INFO"
        tests.wifi_status.value = "Disconnected"
    end
end

local function run_internet_test()
    if not wifi.isAvailable() then
        tests.internet.status = "INFO"
        tests.internet.value = "No WiFi"
        return
    end
    local status = wifi.getStatus()
    if status ~= wifi.STATUS_CONNECTED then
        tests.internet.status = "INFO"
        tests.internet.value = "Not connected"
        return
    end
    if wifi.hasInternet and wifi.hasInternet() then
        tests.internet.status = "PASS"
        tests.internet.value = "Reachable"
    else
        tests.internet.status = "FAIL"
        tests.internet.value = "No internet"
    end
end

local function format_kb(kb)
    if kb >= 1024 then
        return string.format("%.1f MB", kb / 1024)
    else
        return kb .. " KB"
    end
end

local function run_qmi_psram_test()
    local mem = sys.getMemInfo()
    if mem.psram_total > 0 then
        local free_kb = math.floor(mem.psram_free / 1024)
        local total_kb = math.floor(mem.psram_total / 1024)
        tests.qmi_psram.status = "PASS"
        tests.qmi_psram.value = string.format("%dKB free / %dKB", free_kb, total_kb)
    else
        tests.qmi_psram.status = "FAIL"
        tests.qmi_psram.value = "Not detected"
    end
end

local function run_pio_psram_test()
    local mem = sys.getMemInfo()
    if mem.pio_psram_available then
        local size_mb = math.floor(mem.pio_psram_size / (1024 * 1024))
        tests.pio_psram.status = "PASS"
        tests.pio_psram.value = size_mb .. "MB"
    else
        tests.pio_psram.status = "FAIL"
        tests.pio_psram.value = "Not detected"
    end
end

local function run_xip_cache_test()
    -- xip_cache_hit_rate covers the time since the previous getMemInfo()
    -- call: start a measurement, run some code, then read it.
    sys.getMemInfo()
    local x = 0
    for i = 1, 20000 do x = x + i % 7 end
    local rate = sys.getMemInfo().xip_cache_hit_rate
    if rate >= 0 then
        tests.xip_cache.status = "PASS"
        tests.xip_cache.value = rate .. "% hit rate"
    else
        tests.xip_cache.status = "INFO"
        tests.xip_cache.value = "No accesses"
    end
end

local function run_all_tests()
    run_battery_test()
    run_usb_test()
    run_time_test()
    run_log_test()
    run_fs_read_test()
    run_fs_mkdir_test()
    run_fs_write_test()
    run_fs_list_test()
    run_wifi_avail_test()
    run_wifi_status_test()
    run_internet_test()
    run_qmi_psram_test()
    run_pio_psram_test()
    run_xip_cache_test()
end

-- ── SD Card Test (f3write/f3read) ────────────────────────────────────────────

local sd_state = "idle"  -- idle, writing, reading, cleanup, done, error, aborted
local sd_sizes = {4, 16, 64, 256}  -- MB options
local sd_size_idx = 2  -- default 16MB
local sd_total_bytes = 0
local sd_bytes_done = 0
local sd_file_count = 0
local sd_file_cur = 0
local sd_block_cur = 0
local sd_phase_start = 0
local sd_write_speed = 0
local sd_read_speed = 0
local sd_mismatches = 0
local sd_first_mismatch = nil
local sd_error_msg = ""
local sd_free_kb = 0
local sd_total_kb = 0
local sd_test_files = {}
local sd_tick_fh = nil

local SD_BLOCK_SIZE = 4096
local SD_FILE_SIZE = 1024 * 1024  -- 1MB per file
local SD_BLOCKS_PER_FILE = SD_FILE_SIZE / SD_BLOCK_SIZE  -- 256

local function sd_make_block(file_num, block_num)
    -- 16-byte unique signature per block, repeated to fill 4KB.
    -- Fast: 1 string.char + 1 string.rep instead of 4096 iterations.
    -- Each block across all files has a distinct signature, detecting
    -- fake flash aliasing and single-bit errors equally well.
    local a = file_num * 65537 + block_num * 251
    local b = file_num * 131 + block_num * 65537 + 0xDEADBEEF
    local c = a ~ b  -- XOR for extra entropy
    local d = a + b
    local sig = string.char(
         a        & 0xFF, (a >> 8)  & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF,
         b        & 0xFF, (b >> 8)  & 0xFF, (b >> 16) & 0xFF, (b >> 24) & 0xFF,
         c        & 0xFF, (c >> 8)  & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF,
         d        & 0xFF, (d >> 8)  & 0xFF, (d >> 16) & 0xFF, (d >> 24) & 0xFF)
    return string.rep(sig, SD_BLOCK_SIZE // 16)
end

local function sd_cleanup_files()
    for _, name in ipairs(sd_test_files) do
        fs.delete(data_dir .. "/" .. name)
    end
    sd_test_files = {}
end

local function sd_test_file_name(n)
    return string.format("sdtest_%03d.dat", n)
end

local function sd_refresh_disk_info()
    local info = fs.diskInfo()
    if info then
        sd_free_kb = info.free
        sd_total_kb = info.total
    end
end

local function sd_start_test()
    -- Pre-check: ensure card is ready
    if not fs.ensureReady() then
        sd_state = "error"
        sd_error_msg = "SD card not ready"
        return
    end

    sd_refresh_disk_info()

    local test_mb = sd_sizes[sd_size_idx]
    local test_kb = test_mb * 1024
    local safe_free = sd_free_kb - 2048  -- 2MB safety margin
    if safe_free < 1024 then
        sd_state = "error"
        sd_error_msg = "Not enough free space"
        return
    end

    if test_kb > safe_free then
        test_mb = math.floor(safe_free / 1024)
        if test_mb < 1 then
            sd_state = "error"
            sd_error_msg = "Not enough free space"
            return
        end
    end

    sd_file_count = test_mb  -- 1MB per file
    sd_total_bytes = sd_file_count * SD_FILE_SIZE
    sd_bytes_done = 0
    sd_file_cur = 0
    sd_block_cur = 0
    sd_mismatches = 0
    sd_first_mismatch = nil
    sd_write_speed = 0
    sd_read_speed = 0
    sd_error_msg = ""
    sd_test_files = {}
    sd_tick_fh = nil

    -- Ensure data dir exists
    fs.mkdir(data_dir)

    sd_state = "writing"
    sd_phase_start = sys.getTimeMs()
    sd_file_cur = 1
    sd_block_cur = 0
end

-- Drive one block of work per main-loop iteration for responsiveness
local function sd_tick()
    if sd_state == "writing" then
        -- Open file if starting a new one
        if sd_block_cur == 0 then
            local name = sd_test_file_name(sd_file_cur)
            sd_test_files[#sd_test_files + 1] = name
            sd_tick_fh = fs.open(data_dir .. "/" .. name, "w")
            if not sd_tick_fh then
                sd_state = "error"
                sd_error_msg = "Failed to open " .. name .. " for write"
                return
            end
        end

        -- Write a batch of blocks per tick for throughput
        local blocks_per_tick = 16  -- 64KB per tick
        for _ = 1, blocks_per_tick do
            local block = sd_make_block(sd_file_cur, sd_block_cur)
            local written = fs.write(sd_tick_fh, block)
            if written ~= SD_BLOCK_SIZE then
                fs.close(sd_tick_fh)
                sd_tick_fh = nil
                sd_state = "error"
                sd_error_msg = string.format("Write error file %d block %d", sd_file_cur, sd_block_cur)
                return
            end
            sd_bytes_done = sd_bytes_done + SD_BLOCK_SIZE
            sd_block_cur = sd_block_cur + 1

            if sd_block_cur >= SD_BLOCKS_PER_FILE then
                break
            end
        end

        -- File complete?
        if sd_block_cur >= SD_BLOCKS_PER_FILE then
            fs.close(sd_tick_fh)
            sd_tick_fh = nil
            sd_block_cur = 0

            if sd_file_cur >= sd_file_count then
                -- Write phase done, start read phase
                local elapsed = sys.getTimeMs() - sd_phase_start
                if elapsed > 0 then
                    sd_write_speed = math.floor(sd_total_bytes / elapsed)  -- KB/s (bytes/ms = KB/s)
                end
                sd_state = "reading"
                sd_phase_start = sys.getTimeMs()
                sd_bytes_done = 0
                sd_file_cur = 1
                sd_block_cur = 0
            else
                sd_file_cur = sd_file_cur + 1
            end
        end

    elseif sd_state == "reading" then
        -- Open file if starting a new one
        if sd_block_cur == 0 then
            local name = sd_test_file_name(sd_file_cur)
            sd_tick_fh = fs.open(data_dir .. "/" .. name, "r")
            if not sd_tick_fh then
                sd_state = "error"
                sd_error_msg = "Failed to open " .. name .. " for read"
                return
            end
        end

        -- Read a batch of blocks per tick
        local blocks_per_tick = 16
        for _ = 1, blocks_per_tick do
            local data = fs.read(sd_tick_fh, SD_BLOCK_SIZE)
            if not data or #data ~= SD_BLOCK_SIZE then
                fs.close(sd_tick_fh)
                sd_tick_fh = nil
                sd_state = "error"
                sd_error_msg = string.format("Read error file %d block %d", sd_file_cur, sd_block_cur)
                return
            end

            local expected = sd_make_block(sd_file_cur, sd_block_cur)
            if data ~= expected then
                sd_mismatches = sd_mismatches + 1
                if not sd_first_mismatch then
                    -- Find first differing byte
                    for i = 1, #data do
                        if data:byte(i) ~= expected:byte(i) then
                            local abs_offset = (sd_file_cur - 1) * SD_FILE_SIZE + sd_block_cur * SD_BLOCK_SIZE + (i - 1)
                            sd_first_mismatch = string.format(
                                "file %d blk %d byte %d (offset %d): got 0x%02X expect 0x%02X",
                                sd_file_cur, sd_block_cur, i - 1, abs_offset,
                                data:byte(i), expected:byte(i))
                            break
                        end
                    end
                end
            end

            sd_bytes_done = sd_bytes_done + SD_BLOCK_SIZE
            sd_block_cur = sd_block_cur + 1

            if sd_block_cur >= SD_BLOCKS_PER_FILE then
                break
            end
        end

        -- File complete?
        if sd_block_cur >= SD_BLOCKS_PER_FILE then
            fs.close(sd_tick_fh)
            sd_tick_fh = nil
            sd_block_cur = 0

            if sd_file_cur >= sd_file_count then
                -- Read phase done
                local elapsed = sys.getTimeMs() - sd_phase_start
                if elapsed > 0 then
                    sd_read_speed = math.floor(sd_total_bytes / elapsed)
                end
                sd_state = "cleanup"
            else
                sd_file_cur = sd_file_cur + 1
            end
        end

    elseif sd_state == "cleanup" then
        sd_cleanup_files()
        sd_state = "done"
    end
end

local function sd_abort()
    if sd_tick_fh then
        fs.close(sd_tick_fh)
        sd_tick_fh = nil
    end
    sd_cleanup_files()
    sd_state = "aborted"
end

local function sd_format_bytes(b)
    if b >= 1024 * 1024 then
        return string.format("%.1f MB", b / (1024 * 1024))
    elseif b >= 1024 then
        return string.format("%.1f KB", b / 1024)
    else
        return b .. " B"
    end
end

-- ── RAM Test (PIO PSRAM and QMI PSRAM write/verify/speed) ────────────────────

local ram_state = "idle"  -- idle, writing, reading, done, error, aborted
local ram_sizes = {64, 256, 1024, 4096}  -- KB options
local ram_size_idx = 2  -- default 256KB
local ram_target = 1  -- 1 = PIO, 2 = QMI
local ram_target_names = {"PIO", "QMI"}
local ram_total_bytes = 0
local ram_bytes_done = 0
local ram_phase_start = 0
local ram_write_speed = 0
local ram_read_speed = 0
local ram_mismatches = 0
local ram_first_mismatch = nil
local ram_error_msg = ""
local ram_qmi_handle = nil  -- QMI PSRAM alloc handle

-- PIO PSRAM: test after reserved regions (MP3=32KB, Video=256KB, App=288KB+)
local RAM_TEST_BASE = 288 * 1024
local RAM_BLOCK_SIZE = 256  -- bytes per block

local function ram_make_block(block_num)
    local a = block_num * 65537 + 0xCAFEBABE
    local b = block_num * 131 + 0xDEADC0DE
    local c = a ~ b
    local d = a + b
    local sig = string.char(
         a        & 0xFF, (a >> 8)  & 0xFF, (a >> 16) & 0xFF, (a >> 24) & 0xFF,
         b        & 0xFF, (b >> 8)  & 0xFF, (b >> 16) & 0xFF, (b >> 24) & 0xFF,
         c        & 0xFF, (c >> 8)  & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF,
         d        & 0xFF, (d >> 8)  & 0xFF, (d >> 16) & 0xFF, (d >> 24) & 0xFF)
    return string.rep(sig, RAM_BLOCK_SIZE // 16)
end

local function ram_cleanup()
    if ram_qmi_handle then
        sys.qmiPsramFree(ram_qmi_handle)
        ram_qmi_handle = nil
    end
end

local function ram_start_test()
    if ram_target == 1 then
        -- PIO PSRAM
        local psram_size = sys.pioPsramSize()
        if psram_size == 0 then
            ram_state = "error"
            ram_error_msg = "PIO PSRAM not available"
            return
        end
        local test_bytes = ram_sizes[ram_size_idx] * 1024
        local max_test = psram_size - RAM_TEST_BASE
        if test_bytes > max_test then test_bytes = max_test end
        if test_bytes < RAM_BLOCK_SIZE then
            ram_state = "error"
            ram_error_msg = "Not enough PIO PSRAM space"
            return
        end
        ram_total_bytes = test_bytes
    else
        -- QMI PSRAM
        local mem = sys.getMemInfo()
        if mem.psram_total == 0 then
            ram_state = "error"
            ram_error_msg = "QMI PSRAM not available"
            return
        end
        local test_bytes = ram_sizes[ram_size_idx] * 1024
        -- Leave headroom for Lua VM (at least 512KB)
        local max_test = mem.psram_free - 512 * 1024
        if max_test < RAM_BLOCK_SIZE then
            ram_state = "error"
            ram_error_msg = "Not enough free QMI PSRAM"
            return
        end
        if test_bytes > max_test then test_bytes = max_test end
        -- Whole blocks only: qmiPsramWrite is bounds-checked
        test_bytes = test_bytes - test_bytes % RAM_BLOCK_SIZE
        ram_qmi_handle = sys.qmiPsramAlloc(test_bytes)
        if not ram_qmi_handle then
            ram_state = "error"
            ram_error_msg = "QMI PSRAM alloc failed"
            return
        end
        ram_total_bytes = test_bytes
    end

    ram_bytes_done = 0
    ram_mismatches = 0
    ram_first_mismatch = nil
    ram_write_speed = 0
    ram_read_speed = 0
    ram_error_msg = ""

    ram_state = "writing"
    ram_phase_start = sys.getTimeMs()
end

local function ram_tick()
    if ram_state == "writing" then
        local blocks_per_tick = 64  -- 16KB per tick
        for _ = 1, blocks_per_tick do
            if ram_bytes_done >= ram_total_bytes then break end
            local block_num = ram_bytes_done // RAM_BLOCK_SIZE
            local block = ram_make_block(block_num)
            if ram_target == 1 then
                sys.pioPsramWrite(RAM_TEST_BASE + ram_bytes_done, block)
            else
                sys.qmiPsramWrite(ram_qmi_handle, ram_bytes_done, block)
            end
            ram_bytes_done = ram_bytes_done + RAM_BLOCK_SIZE
        end

        if ram_bytes_done >= ram_total_bytes then
            local elapsed = sys.getTimeMs() - ram_phase_start
            if elapsed > 0 then
                ram_write_speed = math.floor(ram_total_bytes / elapsed)
            end
            ram_state = "reading"
            ram_phase_start = sys.getTimeMs()
            ram_bytes_done = 0
        end

    elseif ram_state == "reading" then
        local blocks_per_tick = 64
        for _ = 1, blocks_per_tick do
            if ram_bytes_done >= ram_total_bytes then break end
            local block_num = ram_bytes_done // RAM_BLOCK_SIZE
            local data
            if ram_target == 1 then
                data = sys.pioPsramRead(RAM_TEST_BASE + ram_bytes_done, RAM_BLOCK_SIZE)
            else
                data = sys.qmiPsramRead(ram_qmi_handle, ram_bytes_done, RAM_BLOCK_SIZE)
            end

            if not data or #data ~= RAM_BLOCK_SIZE then
                ram_state = "error"
                ram_error_msg = string.format("Read failed at offset %d", ram_bytes_done)
                ram_cleanup()
                return
            end

            local expected = ram_make_block(block_num)
            if data ~= expected then
                ram_mismatches = ram_mismatches + 1
                if not ram_first_mismatch then
                    for i = 1, #data do
                        if data:byte(i) ~= expected:byte(i) then
                            local abs_addr = ram_bytes_done + i - 1
                            if ram_target == 1 then abs_addr = RAM_TEST_BASE + abs_addr end
                            ram_first_mismatch = string.format(
                                "blk %d byte %d (0x%06X): got 0x%02X expect 0x%02X",
                                block_num, i - 1, abs_addr,
                                data:byte(i), expected:byte(i))
                            break
                        end
                    end
                end
            end

            ram_bytes_done = ram_bytes_done + RAM_BLOCK_SIZE
        end

        if ram_bytes_done >= ram_total_bytes then
            local elapsed = sys.getTimeMs() - ram_phase_start
            if elapsed > 0 then
                ram_read_speed = math.floor(ram_total_bytes / elapsed)
            end
            ram_cleanup()
            ram_state = "done"
        end
    end
end

local function ram_abort()
    ram_cleanup()
    ram_state = "aborted"
end

-- ── Drawing Functions ─────────────────────────────────────────────────────────

local function draw_status_color(status)
    if status == "PASS" then return GOOD
    elseif status == "FAIL" then return WARN
    elseif status == "WARN" then return WARN
    elseif status == "INFO" then return disp.CYAN
    else return DIM
    end
end

local function draw_tabs_demo()
    disp.clear(BG)

    -- drawHeader returns the rows it covers (older firmware: nothing, 29)
    local top = pc.ui.drawHeader("System Test Suite") or 29

    -- Draw tabs with customizable navigation keys
    local new_tab, height = pc.ui.drawTabs(top, tab_labels, active_tab,
                                           nav_keys.prev, nav_keys.next)
    active_tab = new_tab

    -- Content area starts below tabs
    local content_y = top + height + 10

    -- Draw content based on active tab
    if active_tab == 1 then
        -- Tests tab - scrollable test results + system info
        local line_height = 11
        local footer_y = 300
        local max_visible_y = footer_y - 12

        -- Build all rows as {text, color, value, value_color}
        local rows = {}
        for _, key in ipairs(test_order) do
            local test = tests[key]
            local color = draw_status_color(test.status)
            local val = test.value
            if #val > 28 then val = val:sub(1, 25) .. "..." end
            rows[#rows + 1] = {name = test.name, status = test.status, status_color = color, value = val}
        end

        -- Separator
        rows[#rows + 1] = {separator = true}

        -- System info section
        local runtime = sys.getTimeMs() - start_time
        rows[#rows + 1] = {info = "Uptime", value = runtime .. " ms"}

        local mem = sys.getMemInfo()
        rows[#rows + 1] = {info = "SRAM Heap", value = string.format("%d B free / %d B used", mem.sram_free, mem.sram_used)}
        rows[#rows + 1] = {info = "QMI PSRAM", value = string.format("%s free / %s",
            format_kb(math.floor(mem.psram_free / 1024)),
            format_kb(math.floor(mem.psram_total / 1024)))}
        if mem.pio_psram_available then
            local pio_kb = math.floor(mem.pio_psram_size / 1024)
            local pio_reserved_kb = 288  -- MP3 ring (32KB) + video buffer (256KB)
            local pio_free_kb = pio_kb - pio_reserved_kb
            rows[#rows + 1] = {info = "PIO PSRAM", value = string.format("%s free / %s", format_kb(pio_free_kb), format_kb(pio_kb))}
        end
        local disk = fs.diskInfo()
        if disk then
            rows[#rows + 1] = {info = "SD Card", value = string.format("%s free / %s",
                format_kb(disk.free), format_kb(disk.total))}
        end

        -- Display info (moved from Display tab)
        rows[#rows + 1] = {info = "Display", value = "320x320 RGB565"}

        -- Calculate scroll bounds
        local visible_lines = math.floor((max_visible_y - content_y) / line_height)
        local max_scroll = math.max(0, #rows - visible_lines)
        if tests_scroll > max_scroll then tests_scroll = max_scroll end

        -- Draw visible rows
        local y = content_y
        for i = tests_scroll + 1, #rows do
            if y > max_visible_y then break end

            local row = rows[i]
            if row.separator then
                y = y + 2
                disp.drawLine(4, y, 310, y, DIM)
                y = y + 4
            elseif row.name then
                -- Test result row
                disp.drawText(4, y, row.name, FG, BG)
                disp.drawText(80, y, row.status, row.status_color, BG)
                if row.value ~= "" then
                    disp.drawText(120, y, row.value, DIM, BG)
                end
                y = y + line_height
            elseif row.info then
                -- Info row
                disp.drawText(4, y, row.info .. ":", DIM, BG)
                disp.drawText(80, y, row.value, FG, BG)
                y = y + line_height
            end
        end

        -- Scroll indicators
        if tests_scroll > 0 then
            disp.drawText(308, content_y, "^", TITLE, BG)
        end
        if tests_scroll < max_scroll then
            disp.drawText(308, max_visible_y - 8, "v", TITLE, BG)
        end

        -- Footer instructions
        pc.ui.drawFooter("UP/DN:Scroll  R:Rerun  ESC:Exit", nil)

    elseif active_tab == 2 then
        -- Functions tab
        if hello_active then
            -- Hello World demo sub-mode
            local perf = pc.perf
            perf.drawFPS()

            local blink = (hello_frame % 60) < 30
            if blink then
                disp.drawText(100, 140, "* Hello World *", FG, BG)
            end

            local bx = 8 + math.floor(math.abs(math.sin(hello_frame / 60 * math.pi)) * 260)
            local by = 100
            disp.fillRect(bx, by, 20, 20, TITLE)

            local bat = sys.getBattery()
            local bat_str = (bat >= 0) and ("Bat: " .. bat .. "%  ") or ""
            local ms = sys.getTimeMs()
            local secs = math.floor(ms / 1000)
            pc.ui.drawFooter("ESC:Back", bat_str .. "Uptime: " .. secs .. "s")
        else
            -- Function list
            local y = content_y
            for i, item in ipairs(func_items) do
                local prefix = (i == func_sel) and "> " or "  "
                local fg = (i == func_sel) and FG or DIM
                disp.drawText(4, y, prefix .. item.label, fg, BG)
                if i == func_sel then
                    disp.drawText(20, y + 10, item.desc, DIM, BG)
                    y = y + 22
                else
                    y = y + 12
                end
            end

            pc.ui.drawFooter("UP/DN:Select  ENTER:Run  ESC:Exit", nil)
        end

    elseif active_tab == 3 then
        -- Input tab with keytest functionality
        disp.drawText(20, content_y, "Input Tab", TITLE, BG)
        content_y = content_y + 16

        -- Last raw keycode
        disp.drawText(20, content_y, "Last raw:", DIM, BG)
        content_y = content_y + 10
        disp.drawText(80, content_y, string.format("0x%02X  (%d)", last_raw, last_raw), FG, BG)
        content_y = content_y + 12

        -- Last character
        disp.drawText(20, content_y, "Char:", DIM, BG)
        if last_char then
            local label = last_char
            if string.byte(last_char) == 8 then label = "<Bkspc>" end
            disp.drawText(80, content_y, "'" .. label .. "'", FG, BG)
        else
            disp.drawText(80, content_y, "(none)", DIM, BG)
        end
        content_y = content_y + 12

        -- Currently held
        disp.drawText(20, content_y, "Held:", DIM, BG)
        disp.drawText(80, content_y, btn_names(button_state) or "(none)", FG, BG)
        content_y = content_y + 16

        -- Event log
        disp.drawText(20, content_y, "Event log:", DIM, BG)
        content_y = content_y + 10
        for i, entry in ipairs(history) do
            local y = content_y + (i - 1) * 10
            if y < 300 then
                disp.drawText(20, y, entry, i == 1 and FG or DIM, BG)
            end
        end

    elseif active_tab == 4 then
        disp.drawText(20, content_y, "Network Tab", TITLE, BG)
        content_y = content_y + 20
        disp.drawText(20, content_y, "WiFi: " .. tests.wifi_status.value, FG, BG)
        content_y = content_y + 12
        disp.drawText(20, content_y, "Status: " .. tests.wifi_avail.value, FG, BG)

    elseif active_tab == 5 then
        -- SD Card test tab
        local y = content_y

        if sd_state == "idle" then
            disp.drawText(20, y, "SD Card Test (f3write/f3read)", TITLE, BG)
            y = y + 16

            sd_refresh_disk_info()
            disp.drawText(20, y, string.format("Free: %d MB / %d MB",
                math.floor(sd_free_kb / 1024), math.floor(sd_total_kb / 1024)), FG, BG)
            y = y + 14

            disp.drawText(20, y, "Test size:", DIM, BG)
            local size_str = ""
            for i, sz in ipairs(sd_sizes) do
                if i == sd_size_idx then
                    size_str = size_str .. " [" .. sz .. "MB]"
                else
                    size_str = size_str .. "  " .. sz .. "MB "
                end
            end
            disp.drawText(80, y, size_str, FG, BG)
            y = y + 16

            disp.drawText(20, y, "Writes unique patterns to 1MB files,", DIM, BG)
            y = y + 10
            disp.drawText(20, y, "reads back & verifies every byte.", DIM, BG)
            y = y + 10
            disp.drawText(20, y, "Detects fake flash & measures speed.", DIM, BG)
            y = y + 20

            disp.drawText(20, y, "UP/DOWN: size  ENTER: start", GOOD, BG)

            pc.ui.drawFooter("UP/DN:Size  ENTER:Start  ESC:Exit", nil)

        elseif sd_state == "writing" or sd_state == "reading" then
            local phase = sd_state == "writing" and "Writing" or "Verifying"
            disp.drawText(20, y, phase .. "...", TITLE, BG)
            y = y + 14

            disp.drawText(20, y, string.format("File %d / %d", sd_file_cur, sd_file_count), FG, BG)
            y = y + 12

            disp.drawText(20, y, sd_format_bytes(sd_bytes_done) .. " / " .. sd_format_bytes(sd_total_bytes), FG, BG)
            y = y + 16

            -- Progress bar
            local bar_x, bar_w, bar_h = 20, 280, 14
            disp.drawRect(bar_x, y, bar_w, bar_h, DIM)
            local pct = sd_total_bytes > 0 and sd_bytes_done / sd_total_bytes or 0
            local fill = math.floor(pct * (bar_w - 2))
            if fill > 0 then
                disp.fillRect(bar_x + 1, y + 1, fill, bar_h - 2, GOOD)
            end
            disp.drawText(bar_x + bar_w / 2 - 12, y + 3, string.format("%d%%", math.floor(pct * 100)), BG, GOOD)
            y = y + bar_h + 10

            -- Live throughput
            local elapsed = sys.getTimeMs() - sd_phase_start
            if elapsed > 0 and sd_bytes_done > 0 then
                local speed = math.floor(sd_bytes_done / elapsed)  -- KB/s
                disp.drawText(20, y, string.format("Speed: %d KB/s", speed), DIM, BG)
            end

            if sd_state == "reading" and sd_mismatches > 0 then
                y = y + 12
                disp.drawText(20, y, string.format("Mismatches: %d blocks", sd_mismatches), WARN, BG)
            end

            pc.ui.drawFooter("ESC:Abort", nil)

        elseif sd_state == "cleanup" then
            disp.drawText(20, y, "Cleaning up test files...", TITLE, BG)

        elseif sd_state == "done" then
            local passed = sd_mismatches == 0
            disp.drawText(20, y, passed and "PASS" or "FAIL", passed and GOOD or WARN, BG)
            y = y + 16

            disp.drawText(20, y, "Tested: " .. sd_format_bytes(sd_total_bytes), FG, BG)
            y = y + 12
            disp.drawText(20, y, string.format("Files: %d x 1MB", sd_file_count), FG, BG)
            y = y + 12
            disp.drawText(20, y, string.format("Write: %d KB/s", sd_write_speed), FG, BG)
            y = y + 12
            disp.drawText(20, y, string.format("Read:  %d KB/s", sd_read_speed), FG, BG)
            y = y + 12

            if sd_mismatches > 0 then
                disp.drawText(20, y, string.format("Mismatches: %d blocks", sd_mismatches), WARN, BG)
                y = y + 12
                if sd_first_mismatch then
                    disp.drawText(8, y, sd_first_mismatch, WARN, BG)
                    y = y + 10
                end
            else
                disp.drawText(20, y, "All data verified OK", GOOD, BG)
                y = y + 12
            end

            y = y + 8
            disp.drawText(20, y, "ENTER: run again  ESC: exit", DIM, BG)
            pc.ui.drawFooter("ENTER:Again  ESC:Exit", nil)

        elseif sd_state == "aborted" then
            disp.drawText(20, y, "Test aborted", WARN, BG)
            y = y + 16
            disp.drawText(20, y, "Test files cleaned up.", DIM, BG)
            y = y + 16
            disp.drawText(20, y, "ENTER: run again  ESC: exit", DIM, BG)
            pc.ui.drawFooter("ENTER:Again  ESC:Exit", nil)

        elseif sd_state == "error" then
            disp.drawText(20, y, "ERROR", WARN, BG)
            y = y + 16
            disp.drawText(20, y, sd_error_msg, WARN, BG)
            y = y + 16
            disp.drawText(20, y, "ENTER: retry  ESC: exit", DIM, BG)
            pc.ui.drawFooter("ENTER:Retry  ESC:Exit", nil)
        end

    elseif active_tab == 6 then
        -- RAM test tab (PIO PSRAM)
        local y = content_y

        if ram_state == "idle" then
            disp.drawText(20, y, "PSRAM Test", TITLE, BG)
            y = y + 16

            -- Target toggle
            disp.drawText(20, y, "Target:", DIM, BG)
            for i, name in ipairs(ram_target_names) do
                local lbl = (i == ram_target) and ("[" .. name .. "]") or (" " .. name .. " ")
                local col = (i == ram_target) and FG or DIM
                disp.drawText(68 + (i - 1) * 50, y, lbl, col, BG)
            end
            y = y + 14

            -- Show available space for selected target
            if ram_target == 1 then
                local psram_size = sys.pioPsramSize()
                if psram_size > 0 then
                    local avail_kb = math.floor((psram_size - RAM_TEST_BASE) / 1024)
                    disp.drawText(20, y, string.format("Available: %d KB (of %d MB)",
                        avail_kb, math.floor(psram_size / (1024 * 1024))), FG, BG)
                else
                    disp.drawText(20, y, "PIO PSRAM not detected!", WARN, BG)
                end
            else
                local mem = sys.getMemInfo()
                if mem.psram_total > 0 then
                    local free_kb = math.floor(mem.psram_free / 1024)
                    local total_kb = math.floor(mem.psram_total / 1024)
                    disp.drawText(20, y, string.format("Available: %d KB (of %d KB)",
                        free_kb, total_kb), FG, BG)
                else
                    disp.drawText(20, y, "QMI PSRAM not detected!", WARN, BG)
                end
            end
            y = y + 14

            disp.drawText(20, y, "Test size:", DIM, BG)
            local size_str = ""
            for i, sz in ipairs(ram_sizes) do
                if i == ram_size_idx then
                    size_str = size_str .. " [" .. sz .. "KB]"
                else
                    size_str = size_str .. "  " .. sz .. "KB "
                end
            end
            disp.drawText(80, y, size_str, FG, BG)
            y = y + 16

            if ram_target == 1 then
                disp.drawText(20, y, "Writes unique patterns via PIO SPI,", DIM, BG)
                y = y + 10
                disp.drawText(20, y, "reads back & verifies every byte.", DIM, BG)
                y = y + 10
                disp.drawText(20, y, "Tests mainboard PSRAM chip.", DIM, BG)
            else
                disp.drawText(20, y, "Writes unique patterns to QMI PSRAM,", DIM, BG)
                y = y + 10
                disp.drawText(20, y, "reads back & verifies every byte.", DIM, BG)
                y = y + 10
                disp.drawText(20, y, "Tests Pimoroni onboard PSRAM.", DIM, BG)
            end
            y = y + 20

            disp.drawText(20, y, "T: target  UP/DN: size  ENTER: start", GOOD, BG)

            pc.ui.drawFooter("T:Target  UP/DN:Size  ENTER:Start", nil)

        elseif ram_state == "writing" or ram_state == "reading" then
            local phase = ram_state == "writing" and "Writing" or "Verifying"
            disp.drawText(20, y, phase .. " " .. ram_target_names[ram_target] .. " PSRAM...", TITLE, BG)
            y = y + 14

            disp.drawText(20, y, sd_format_bytes(ram_bytes_done) .. " / " .. sd_format_bytes(ram_total_bytes), FG, BG)
            y = y + 16

            -- Progress bar
            local bar_x, bar_w, bar_h = 20, 280, 14
            disp.drawRect(bar_x, y, bar_w, bar_h, DIM)
            local pct = ram_total_bytes > 0 and ram_bytes_done / ram_total_bytes or 0
            local fill = math.floor(pct * (bar_w - 2))
            if fill > 0 then
                disp.fillRect(bar_x + 1, y + 1, fill, bar_h - 2, GOOD)
            end
            disp.drawText(bar_x + bar_w / 2 - 12, y + 3, string.format("%d%%", math.floor(pct * 100)), BG, GOOD)
            y = y + bar_h + 10

            -- Live throughput
            local elapsed = sys.getTimeMs() - ram_phase_start
            if elapsed > 0 and ram_bytes_done > 0 then
                local speed = math.floor(ram_bytes_done / elapsed)
                disp.drawText(20, y, string.format("Speed: %d KB/s", speed), DIM, BG)
            end

            if ram_state == "reading" and ram_mismatches > 0 then
                y = y + 12
                disp.drawText(20, y, string.format("Mismatches: %d blocks", ram_mismatches), WARN, BG)
            end

            pc.ui.drawFooter("ESC:Abort", nil)

        elseif ram_state == "done" then
            local passed = ram_mismatches == 0
            disp.drawText(20, y, passed and "PASS" or "FAIL", passed and GOOD or WARN, BG)
            y = y + 16

            disp.drawText(20, y, "Tested: " .. sd_format_bytes(ram_total_bytes), FG, BG)
            y = y + 12
            disp.drawText(20, y, string.format("Write: %d KB/s", ram_write_speed), FG, BG)
            y = y + 12
            disp.drawText(20, y, string.format("Read:  %d KB/s", ram_read_speed), FG, BG)
            y = y + 12

            if ram_mismatches > 0 then
                disp.drawText(20, y, string.format("Mismatches: %d blocks", ram_mismatches), WARN, BG)
                y = y + 12
                if ram_first_mismatch then
                    disp.drawText(8, y, ram_first_mismatch, WARN, BG)
                    y = y + 10
                end
            else
                disp.drawText(20, y, "All data verified OK", GOOD, BG)
                y = y + 12
            end

            y = y + 8
            disp.drawText(20, y, "ENTER: run again  ESC: exit", DIM, BG)
            pc.ui.drawFooter("ENTER:Again  ESC:Exit", nil)

        elseif ram_state == "aborted" then
            disp.drawText(20, y, "Test aborted", WARN, BG)
            y = y + 16
            disp.drawText(20, y, "ENTER: run again  ESC: exit", DIM, BG)
            pc.ui.drawFooter("ENTER:Again  ESC:Exit", nil)

        elseif ram_state == "error" then
            disp.drawText(20, y, "ERROR", WARN, BG)
            y = y + 16
            disp.drawText(20, y, ram_error_msg, WARN, BG)
            y = y + 16
            disp.drawText(20, y, "ENTER: retry  ESC: exit", DIM, BG)
            pc.ui.drawFooter("ENTER:Retry  ESC:Exit", nil)
        end
    end

    -- Footer with instructions for tabs that don't draw their own
    if active_tab >= 3 and active_tab <= 4 then
        pc.ui.drawFooter("F1:Exit  T:ToggleKeys  ESC:Back", nil)
    end
end

-- ── Main Loop ─────────────────────────────────────────────────────────────────

start_time = sys.getTimeMs()

-- Run all tests once at startup
run_all_tests()

while true do
    input.update()
    local pressed = input.getButtonsPressed()
    local released = input.getButtonsReleased()
    button_state = input.getButtons()

    -- Track raw key and character for Input tab
    -- Get char first so it's available when we check raw key
    last_char = input.getChar()
    local raw = input.getRawKey()
    if raw ~= 0 and raw ~= last_raw then
        last_raw = raw
        local label = ""
        if last_char then
            if string.byte(last_char) == 8 then
                label = "<Bkspc>"
            else
                label = string.format("'%s'", last_char)
            end
        elseif btn_names(pressed) then
            label = btn_names(pressed)
        else
            -- Raw key but no char and no button name - use raw key code
            label = string.format("[0x%02X]", raw)
        end
        push_history(string.format("0x%02X  %s", raw, label))
    end

    -- Drive SD card test forward regardless of active tab
    if sd_state == "writing" or sd_state == "reading" or sd_state == "cleanup" then
        -- ESC aborts the test (from any tab)
        if pressed & input.BTN_ESC ~= 0 then
            sd_abort()
        else
            sd_tick()
        end
    -- Drive RAM test forward regardless of active tab
    elseif ram_state == "writing" or ram_state == "reading" then
        if pressed & input.BTN_ESC ~= 0 then
            ram_abort()
        else
            ram_tick()
        end
    elseif active_tab == 2 and not hello_active then
        -- Functions tab list input
        if pressed & input.BTN_ESC ~= 0 then
            return
        end
        if pressed & input.BTN_UP ~= 0 then
            func_sel = func_sel > 1 and func_sel - 1 or #func_items
        end
        if pressed & input.BTN_DOWN ~= 0 then
            func_sel = func_sel < #func_items and func_sel + 1 or 1
        end
        if pressed & input.BTN_ENTER ~= 0 then
            if func_sel == 1 then
                -- Hello World Demo
                hello_active = true
                hello_frame = 0
            elseif func_sel == 2 then
                error("Test crash message from systest app")
            elseif func_sel == 3 then
                local x = nil
                local _ = x.foo
            elseif func_sel == 4 then
                local function recurse() recurse() end
                recurse()
            elseif func_sel == 5 then
                disp.clear(BG)
                disp.drawText(4, 4, "Triggering HardFault...", WARN, BG)
                disp.drawText(4, 20, "Device will reboot.", FG, BG)
                disp.flush()
                sys.sleep(500)
                if sys.triggerFault then
                    sys.triggerFault()
                else
                    error("triggerFault not available (rebuild firmware)")
                end
            end
        end
    elseif active_tab == 2 and hello_active then
        -- Hello demo sub-mode: ESC exits back to list
        if pressed & input.BTN_ESC ~= 0 then
            hello_active = false
        end
        hello_frame = hello_frame + 1
    elseif active_tab == 5 then
        -- SD Card tab-specific input when not actively testing
        if sd_state == "idle" then
            if pressed & input.BTN_ESC ~= 0 then
                return
            end
            if pressed & input.BTN_UP ~= 0 then
                sd_size_idx = sd_size_idx > 1 and sd_size_idx - 1 or #sd_sizes
            end
            if pressed & input.BTN_DOWN ~= 0 then
                sd_size_idx = sd_size_idx < #sd_sizes and sd_size_idx + 1 or 1
            end
            if pressed & input.BTN_ENTER ~= 0 then
                sd_start_test()
            end
        elseif sd_state == "done" or sd_state == "aborted" or sd_state == "error" then
            if pressed & input.BTN_ESC ~= 0 then
                return
            end
            if pressed & input.BTN_ENTER ~= 0 then
                sd_state = "idle"
            end
        end
    elseif active_tab == 6 then
        -- RAM tab-specific input when not actively testing
        if ram_state == "idle" then
            if pressed & input.BTN_ESC ~= 0 then
                return
            end
            if last_char and last_char:upper() == "T" then
                ram_target = ram_target == 1 and 2 or 1
            end
            if pressed & input.BTN_UP ~= 0 then
                ram_size_idx = ram_size_idx > 1 and ram_size_idx - 1 or #ram_sizes
            end
            if pressed & input.BTN_DOWN ~= 0 then
                ram_size_idx = ram_size_idx < #ram_sizes and ram_size_idx + 1 or 1
            end
            if pressed & input.BTN_ENTER ~= 0 then
                ram_start_test()
            end
        elseif ram_state == "done" or ram_state == "aborted" or ram_state == "error" then
            if pressed & input.BTN_ESC ~= 0 then
                return
            end
            if pressed & input.BTN_ENTER ~= 0 then
                ram_state = "idle"
            end
        end
    else
        -- Normal tab input handling
        if pressed & input.BTN_ESC ~= 0 then
            return
        end

        -- Tests tab: UP/DOWN scroll, R/ENTER rerun
        if active_tab == 1 then
            if pressed & input.BTN_UP ~= 0 then
                tests_scroll = math.max(0, tests_scroll - 1)
            end
            if pressed & input.BTN_DOWN ~= 0 then
                tests_scroll = tests_scroll + 1  -- clamped in draw
            end
            if pressed & input.BTN_ENTER ~= 0 or (last_char and last_char:upper() == "R") then
                run_all_tests()
                last_raw = 0
                last_char = nil
            end
        end

        -- T: Toggle navigation keys between Left/Right and Up/Down
        if last_char and last_char:upper() == "T" then
            if nav_keys.prev == input.BTN_LEFT then
                nav_keys.prev = input.BTN_UP
                nav_keys.next = input.BTN_DOWN
            else
                nav_keys.prev = input.BTN_LEFT
                nav_keys.next = input.BTN_RIGHT
            end
        end
    end

    draw_tabs_demo()

    disp.flush()
    -- During active tests, minimal sleep for throughput
    if sd_state == "writing" or sd_state == "reading"
       or ram_state == "writing" or ram_state == "reading" then
        sys.sleep(1)
    else
        sys.sleep(16)
    end
end
