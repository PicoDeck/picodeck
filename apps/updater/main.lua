-- PicoDeck System Updater
-- Downloads and applies firmware updates from GitHub Releases

local display = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local net = picocalc.network
local fs = picocalc.fs
local ui = picocalc.ui
local gp = picocalc.gamepad

-- The key bound to a gamepad button, for hints (read at draw time).
local function pad_key(btn) return gp.getLabel(btn) or "?" end

-- GitHub Releases API configuration
local GH_API_HOST  = "api.github.com"
local GH_REPO      = "PicoDeck/picodeck"
local GH_API_PATH  = "/repos/" .. GH_REPO .. "/releases/latest"
local BIN_FILENAME  = "picodeck.bin"
local HASH_FILENAME = "picodeck.sha256"
local SIG_FILENAME  = "picodeck.sig"
local BIN_PATH      = "/system/update.bin"
local HASH_PATH     = "/system/update.sha256"
local SIG_PATH      = "/system/update.sig"
local MAX_REDIRECTS = 3

-- Screen dimensions
local W, H = display.getWidth(), display.getHeight()

-- Colors
local BLACK = 0x0000
local WHITE = 0xFFFF
local RED = 0xF800
local GREEN = 0x07E0
local YELLOW = 0xFFE0
local CYAN = 0x07FF
local GRAY = 0x8410

-- Screens
local SCR_MAIN = 1
local SCR_CHECKING = 2
local SCR_AVAILABLE = 3
local SCR_DOWNLOADING = 4
local SCR_DONE = 5
local SCR_UP_TO_DATE = 6
local SCR_CONFIRM = 7

local current_screen = SCR_MAIN
local error_msg = ""
local current_conn = nil

-- Update info from server
local remote_version = nil
local remote_tag = nil
local remote_size = 0
local remote_changelog = nil
local remote_bin_url = nil
local remote_hash_url = nil
local remote_sig_url = nil

-- Download state
local download_received = 0
local download_total = 0
local download_file = nil
local download_complete = false
local download_conn_id = 0  -- incremented per connection to detect stale callbacks

-- Retry state
local MAX_DOWNLOAD_RETRIES = 3
local download_retries = 0
local download_retry_needed = false
local download_retry_url = nil  -- full URL for retrying

-- ── Helpers ─────────────────────────────────────────────────────────────────

local function draw_centered(y, text, fg, bg)
    local tw = display.textWidth(text)
    display.drawText(math.floor((W - tw) / 2), y, text, fg, bg or BLACK)
end

local function wrap_text(text, max_width)
    if not text or #text == 0 then
        return function() end
    end

    local function do_wrap()
        local lines = {}
        local line = ""
        
        for word in text:gmatch("%S+") do
            local test = (line == "") and word or (line .. " " .. word)
            local test_w = display.textWidth(test)
            if test_w and test_w > max_width then
                if line ~= "" then
                    table.insert(lines, line)
                end
                line = word
                while display.textWidth(line) and display.textWidth(line) > max_width do
                    local cut = #line
                    while cut > 1 and display.textWidth(line:sub(1, cut)) 
                          and display.textWidth(line:sub(1, cut)) > max_width do
                        cut = cut - 1
                    end
                    table.insert(lines, line:sub(1, cut))
                    line = line:sub(cut + 1)
                end
            else
                line = test
            end
        end
        if line ~= "" then
            table.insert(lines, line)
        end
        
        local i = 0
        return function()
            i = i + 1
            return lines[i]
        end
    end

    return do_wrap()
end

local function version_newer(remote, local_ver)
    -- Simple semver comparison: "1.0.1" > "1.0.0"
    local function parse(v)
        local parts = {}
        for n in v:gmatch("(%d+)") do
            parts[#parts + 1] = tonumber(n)
        end
        return parts
    end
    local r = parse(remote)
    local l = parse(local_ver)
    for i = 1, math.max(#r, #l) do
        local rv = r[i] or 0
        local lv = l[i] or 0
        if rv > lv then return true end
        if rv < lv then return false end
    end
    return false
end

-- Simple JSON value extraction (no full parser needed)
local function json_get(json, key)
    -- Try string value first
    local val = json:match('"' .. key .. '"%s*:%s*"([^"]*)"')
    if val then return val end
    -- Try number value
    val = json:match('"' .. key .. '"%s*:%s*(%d+)')
    if val then return val end
    return nil
end

-- Parse a URL into host, port, ssl, path
local function parse_url(url)
    local host, path
    local ssl = false
    local port = 80

    if url:match("^https://") then
        ssl = true
        port = 443
        local rest = url:sub(9)
        host = rest:match("^([^/]+)")
        path = rest:match("^[^/]+(/.*)") or "/"
    elseif url:match("^http://") then
        local rest = url:sub(8)
        host = rest:match("^([^/]+)")
        path = rest:match("^[^/]+(/.*)") or "/"
    else
        return nil
    end

    -- Extract port from host if present
    local h, p = host:match("^(.+):(%d+)$")
    if h then
        host = h
        port = tonumber(p)
    end

    return host, port, ssl, path
end

-- ── Network: Check for updates ──────────────────────────────────────────────

local function check_for_update()
    current_screen = SCR_CHECKING

    local status = net.getStatus()
    if status ~= net.kStatusConnected then
        error_msg = "WiFi not connected"
        sys.log("UPDATER: " .. error_msg)
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        return
    end

    local conn = net.http.new(GH_API_HOST, 443, true)
    if not conn then
        error_msg = "Cannot connect to GitHub API."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        return
    end

    conn:setConnectTimeout(15)
    conn:setReadTimeout(15)
    conn:setReadBufferSize(8192)
    current_conn = conn

    local body = ""

    conn:setRequestCallback(function()
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            local data = conn:read()
            body = body .. data
        end
    end)

    conn:setRequestCompleteCallback(function()
        local http_status = conn:getResponseStatus()
        print("[UPDATER] API response: status=" .. tostring(http_status) .. " body_len=" .. tostring(#body))
        conn:close()
        current_conn = nil

        if http_status ~= 200 then
            error_msg = "GitHub API returned HTTP " .. tostring(http_status)
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
            return
        end

        -- Parse release info
        local tag = json_get(body, "tag_name")
        print("[UPDATER] tag_name=" .. tostring(tag))
        if not tag then
            error_msg = "Invalid response from GitHub API."
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
            return
        end

        -- Strip leading 'v' for version comparison
        remote_tag = tag
        remote_version = tag:match("^v(.+)") or tag

        -- Extract changelog from release body
        remote_changelog = json_get(body, "body")

        -- Extract binary size from assets array
        local size_str = body:match(
            '"name"%s*:%s*"picodeck%.bin".-"size"%s*:%s*(%d+)')
        remote_size = tonumber(size_str or "0")

        -- Construct download URLs from tag (deterministic pattern)
        local dl_base = "https://github.com/" .. GH_REPO ..
            "/releases/download/" .. tag .. "/"
        remote_bin_url = dl_base .. BIN_FILENAME
        remote_hash_url = dl_base .. HASH_FILENAME
        remote_sig_url = dl_base .. SIG_FILENAME

        local local_ver = sys.getVersion()
        if version_newer(remote_version, local_ver) then
            current_screen = SCR_AVAILABLE
        else
            current_screen = SCR_UP_TO_DATE
        end
    end)

    conn:setConnectionClosedCallback(function()
        current_conn = nil
        if current_screen == SCR_CHECKING then
            error_msg = "Connection closed unexpectedly."
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
        end
    end)

    local headers = {
        ["User-Agent"] = "PicoDeck-Updater/1.0",
        ["Accept"] = "application/vnd.github+json",
    }

    if not conn:get(GH_API_PATH, headers) then
        error_msg = "Failed to send request."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        current_conn = nil
    end
end

-- ── Network: Download with redirect following ───────────────────────────────

-- Fetch a small release file (checksum or signature) into memory, following
-- redirects, then write `extract(body)` to `dest`.  `on_done()` runs either
-- way: without a file applyUpdate refuses and says why.
local function download_small_file(url, dest, extract, on_done, redirect_count)
    redirect_count = redirect_count or 0
    if redirect_count >= MAX_REDIRECTS then
        on_done()
        return
    end

    local host, port, ssl, path = parse_url(url)
    if not host then
        on_done()
        return
    end

    local conn = net.http.new(host, port, ssl)
    if not conn then
        on_done()
        return
    end

    conn:setConnectTimeout(15)
    conn:setReadTimeout(15)
    conn:setReadBufferSize(512)
    current_conn = conn

    local body = ""
    local finished = false

    conn:setRequestCallback(function()
        local avail = conn:getBytesAvailable()
        if avail > 0 and #body < 1024 then
            body = body .. conn:read()
        end
    end)

    conn:setRequestCompleteCallback(function()
        local http_status = conn:getResponseStatus()

        local resp_headers = conn:getResponseHeaders()
        finished = true
        conn:close()
        current_conn = nil

        if http_status and http_status >= 300 and http_status < 400 then
            -- Follow redirect
            local location = resp_headers and resp_headers["location"]
            if location then
                download_small_file(location, dest, extract, on_done,
                                    redirect_count + 1)
                return
            end
        end

        local data = (http_status == 200) and extract(body) or nil
        if data then
            local f = fs.open(dest, "w")
            if f then
                fs.write(f, data)
                fs.close(f)
            end
        end
        on_done()
    end)

    conn:setConnectionClosedCallback(function()
        current_conn = nil
        if not finished then
            finished = true
            on_done()
        end
    end)

    if not conn:get(path, {["User-Agent"] = "PicoDeck-Updater/1.0"}) then
        on_done()
    end
end

-- Checksum (64 hex digits) then signature (DER ECDSA P-256), then Done.
local function download_hash_file(url)
    local function finish_download()
        current_screen = SCR_DONE
    end
    download_small_file(url, HASH_PATH, function(body)
        local hash = body:match("^%s*(%x+)%s*$")
        return (hash and #hash == 64) and hash or nil
    end, function()
        if not remote_sig_url then
            finish_download()
            return
        end
        download_small_file(remote_sig_url, SIG_PATH, function(body)
            if #body >= 8 and #body <= 128 and body:byte(1) == 0x30 then
                return body
            end
            return nil
        end, finish_download)
    end)
end

-- Fallback for servers that don't support Range requests
local function download_fallback(host, port, ssl, path)
    -- Close any previous connection before starting a new one
    if current_conn then
        current_conn:close()
        current_conn = nil
    end

    local conn = net.http.new(host, port, ssl)
    if not conn then
        error_msg = "Cannot connect to download server."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        return
    end

    conn:setConnectTimeout(30)
    conn:setReadTimeout(60)
    -- Large ring (PSRAM): nothing slows the sender on the device; 32 KB
    -- if the heap cannot spare it.
    if not conn:setReadBufferSize(256 * 1024) and
       not conn:setReadBufferSize(32 * 1024) then
        error_msg = "Failed to allocate download buffer"
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        conn:close()
        return
    end
    current_conn = conn
    download_complete = false

    -- Capture connection identity to detect stale callbacks
    download_conn_id = download_conn_id + 1
    local my_conn_id = download_conn_id

    -- Buffer download data in PSRAM instead of writing to SD during download.
    -- Writing to SD while WiFi is active causes 3.3V rail droop (combined
    -- CYW43 + SD flash programming current) → card reset (R1=0x3f) on every
    -- write.  Buffering in PSRAM and flushing after the connection closes
    -- eliminates the concurrent power draw.
    local chunks = {}

    conn:setRequestCallback(function()
        if my_conn_id ~= download_conn_id then return end
        if download_complete then return end

        local status = conn:getResponseStatus()
        if status and status ~= 200 then return end

        -- Drain ALL available data from ring buffer into memory
        while true do
            local avail = conn:getBytesAvailable()
            if avail <= 0 then break end
            local data = conn:read()
            if not data or #data == 0 then break end

            -- Cap data at Content-Length to avoid extra bytes
            if download_total > 0 then
                local remaining = download_total - download_received
                if remaining <= 0 then break end
                if #data > remaining then
                    data = data:sub(1, remaining)
                end
            end

            chunks[#chunks + 1] = data
            download_received = download_received + #data
        end
    end)

    conn:setRequestCompleteCallback(function()
        if my_conn_id ~= download_conn_id then return end

        -- Capture status BEFORE closing connection
        local http_status = conn:getResponseStatus()
        local conn_err = conn:getError()
        print("[UPDATER] Complete: status=" .. tostring(http_status) ..
              " received=" .. download_received .. " total=" .. download_total ..
              " err=" .. tostring(conn_err))

        if http_status and http_status >= 300 and http_status < 400 then
            local resp_headers = conn:getResponseHeaders()
            local location = resp_headers and resp_headers["location"]
            if location then
                conn:close()
                if current_conn == conn then current_conn = nil end
                chunks = {}
                local h, p, s, pa = parse_url(location)
                if h then
                    download_fallback(h, p, s, pa)
                else
                    error_msg = "Invalid redirect URL."
                    ui.toast(error_msg, ui.TOAST_ERROR)
                    current_screen = SCR_MAIN
                end
                return
            end
        end

        -- If connection had an error, signal retry
        if conn_err then
            print("[UPDATER] Download failed with error: " .. conn_err)
            conn:close()
            if current_conn == conn then current_conn = nil end
            chunks = {}
            download_retry_needed = true
            return
        end

        -- Drain ALL remaining buffered data into memory
        while true do
            local avail = conn:getBytesAvailable()
            if avail <= 0 then break end
            local data = conn:read()
            if not data or #data == 0 then break end

            if download_total > 0 then
                local remaining = download_total - download_received
                if remaining <= 0 then break end
                if #data > remaining then
                    data = data:sub(1, remaining)
                end
            end

            chunks[#chunks + 1] = data
            download_received = download_received + #data
        end

        -- Check HTTP status before writing to SD
        if http_status and http_status ~= 200 then
            conn:close()
            if current_conn == conn then current_conn = nil end
            chunks = {}
            error_msg = "Download failed: HTTP " .. tostring(http_status)
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
            return
        end

        -- Verify we received the expected amount
        if download_total > 0 and download_received ~= download_total then
            print("[UPDATER] Size mismatch: received=" .. download_received ..
                  " expected=" .. download_total)
            conn:close()
            if current_conn == conn then current_conn = nil end
            chunks = {}
            download_retry_needed = true
            return
        end

        if download_received < 256 then
            print("[UPDATER] Download too small (" .. download_received .. " bytes)")
            conn:close()
            if current_conn == conn then current_conn = nil end
            chunks = {}
            download_retry_needed = true
            return
        end

        -- Fully shut down WiFi and pause Core 1 before writing to SD.
        -- Matches the USB MSC pattern: CYW43 transient current during
        -- wifi_poll() causes voltage sag → SD card resets (R1=0x3f).
        -- Per-sector pause/resume is counterproductive (gives Core 1
        -- brief activity windows between writes); pausing once for the
        -- entire batch eliminates the interference completely.
        conn:close()
        if current_conn == conn then current_conn = nil end

        -- Let Core 1 process the connection close
        sys.sleep(100)

        -- Disconnect WiFi to reduce CYW43 baseline current draw.
        -- wifi.disconnect() is async (queues to Core 1 ring buffer).
        -- Poll for actual hardware deassociation before proceeding.
        picocalc.wifi.disconnect()
        local hw_start = sys.getTimeMs()
        while not picocalc.network.isHwDisconnected() do
            sys.sleep(10)
            if sys.getTimeMs() - hw_start > 2000 then
                print("[UPDATER] WARNING: WiFi hardware disconnect timed out")
                break
            end
        end

        -- Pause Core 1 entirely (WiFi, audio, HTTP polling all stop)
        local paused = sys.pauseBackground()
        if not paused then
            print("[UPDATER] WARNING: Core 1 did not acknowledge pause")
        end

        -- Drop SPI0 to 1 MHz — immune to CYW43 EMI even with radio
        -- stuck active (mg_wifi_disconnect only deassociates, radio
        -- stays powered in RX/scanning mode).
        fs.setSlowMode(true)

        -- With Core 1 stopped, verify SD card is responsive
        if not fs.ensureReady() then
            print("[UPDATER] SD card not responding after recovery")
            fs.setSlowMode(false)
            sys.resumeBackground()
            chunks = {}
            error_msg = "SD card not responding"
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
            return
        end

        -- Flush buffered data to SD card (Core 1 is paused for entire batch)
        print("[UPDATER] Writing " .. download_received .. " bytes to SD...")
        download_file = fs.open(BIN_PATH, "w")
        if not download_file then
            fs.setSlowMode(false)
            sys.resumeBackground()
            chunks = {}
            error_msg = "Failed to open file for writing."
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
            return
        end

        local write_ok = true
        for i = 1, #chunks do
            local written = fs.write(download_file, chunks[i])
            if not written or written < 0 then
                print("[UPDATER] SD write failed at chunk " .. i ..
                      " (written=" .. tostring(written) .. ")")
                write_ok = false
                break
            end
        end
        fs.close(download_file)
        download_file = nil
        chunks = {}  -- free PSRAM

        -- Restore full SPI0 speed and resume Core 1
        fs.setSlowMode(false)
        sys.resumeBackground()

        if not write_ok then
            fs.delete(BIN_PATH)
            download_retry_needed = true
            return
        end

        -- Verify file size on disk
        local actual_size = fs.size(BIN_PATH)
        print("[UPDATER] Verification: actual_size=" .. tostring(actual_size) ..
              " expected=" .. tostring(download_total))
        if download_total > 0 and actual_size ~= download_total then
            print("[UPDATER] Size mismatch on disk, will retry")
            fs.delete(BIN_PATH)
            download_retry_needed = true
            return
        end

        if not actual_size or actual_size < 256 then
            print("[UPDATER] File empty or too small, will retry")
            fs.delete(BIN_PATH)
            download_retry_needed = true
            return
        end

        download_complete = true
        current_screen = SCR_DONE
        print("[UPDATER] Firmware saved successfully")

        if remote_hash_url then
            download_hash_file(remote_hash_url)
        end
    end)

    conn:setHeadersReadCallback(function()
        if my_conn_id ~= download_conn_id then return end
        local hdrs = conn:getResponseHeaders()
        if hdrs and hdrs["content-length"] then
            local cl = tonumber(hdrs["content-length"])
            if cl and cl > 0 then
                download_total = cl
                print("[DEBUG] Headers: Content-Length=" .. tostring(download_total))
            end
        end
    end)

    conn:setConnectionClosedCallback(function()
        if my_conn_id ~= download_conn_id then return end  -- stale
        if current_conn == conn then current_conn = nil end

        -- If download already completed successfully, nothing to do
        if download_complete then return end

        -- If we're not in downloading state, already handled
        if current_screen ~= SCR_DOWNLOADING then return end

        -- Connection closed unexpectedly during download
        chunks = {}
        error_msg = "Download interrupted."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
    end)

    if not conn:get(path, {["User-Agent"] = "PicoDeck-Updater/1.0"}) then
        error_msg = "Failed to start download."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        current_conn = nil
    end
end

local function fetch_firmware(url, redirect_count)
    redirect_count = redirect_count or 0
    if redirect_count >= MAX_REDIRECTS then
        error_msg = "Too many redirects."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        return
    end

    local host, port, ssl, path = parse_url(url)
    if not host then
        error_msg = "Invalid download URL."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        return
    end

    -- Streaming download: 256 KB PSRAM receive ring, 32 KB if that fails
    download_fallback(host, port, ssl, path)
end

local function start_download()
    current_screen = SCR_DOWNLOADING
    download_received = 0
    download_total = remote_size
    download_file = nil
    download_retries = 0
    download_retry_needed = false
    download_retry_url = remote_bin_url

    -- Delete old files to ensure we start fresh (a stale checksum or
    -- signature would only make the OS reject the new image)
    if fs.exists(BIN_PATH) then
        fs.delete(BIN_PATH)
    end
    if fs.exists(HASH_PATH) then
        fs.delete(HASH_PATH)
    end
    if fs.exists(SIG_PATH) then
        fs.delete(SIG_PATH)
    end
    print("[UPDATER] start_download: remote_size=" .. tostring(remote_size) ..
          " url=" .. tostring(remote_bin_url))

    if not remote_bin_url then
        error_msg = "No download URL available."
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_MAIN
        return
    end

    fetch_firmware(remote_bin_url)
end

-- ── Drawing ─────────────────────────────────────────────────────────────────

local function draw_main()
    display.clear(BLACK)
    ui.drawHeader("System Update")

    local ver = sys.getVersion()
    display.drawText(8, 50, "Current firmware:", GRAY, BLACK)
    display.drawText(8, 66, "  Version " .. ver, WHITE, BLACK)

    draw_centered(110, "Press " .. pad_key(gp.PAD_A) .. " to check", CYAN, BLACK)
    draw_centered(126, "for updates", CYAN, BLACK)

    ui.drawFooter("ESC: Exit", "")
    display.flush()
end

local function draw_checking()
    display.clear(BLACK)
    ui.drawHeader("System Update")
    ui.drawSpinner(W / 2, 140, 12, frame)
    draw_centered(165, "Checking for updates...", WHITE, BLACK)
    ui.drawFooter("ESC: Cancel", "")
    display.flush()
end

local function draw_up_to_date()
    display.clear(BLACK)
    ui.drawHeader("System Update")
    draw_centered(130, "Firmware is up to date!", GREEN, BLACK)
    local ver = sys.getVersion()
    draw_centered(150, "Version " .. ver, GRAY, BLACK)
    draw_centered(190, pad_key(gp.PAD_A) .. ": Reflash current version", CYAN, BLACK)
    ui.drawFooter("ESC: Back", "")
    display.flush()
end

local function draw_available()
    display.clear(BLACK)
    ui.drawHeader("System Update")

    display.drawText(8, 50, "Update available!", GREEN, BLACK)

    local ver = sys.getVersion()
    display.drawText(8, 74, "Current: " .. ver, GRAY, BLACK)

    local y = 90
    local max_w = W - 16
    for wrapped in wrap_text("New:     " .. (remote_version or "?"), max_w) do
        display.drawText(8, y, wrapped, WHITE, BLACK)
        y = y + 14
    end

    if remote_size > 0 then
        local size_kb = math.floor(remote_size / 1024)
        display.drawText(8, 110, "Size: " .. size_kb .. " KB", GRAY, BLACK)
    end

    if remote_changelog then
        display.drawText(8, 134, "Changes:", YELLOW, BLACK)
        local y = 150
        local max_w = W - 16
        for wrapped in wrap_text(remote_changelog, max_w) do
            display.drawText(8, y, wrapped, WHITE, BLACK)
            y = y + 12
            if y > H - 50 then break end
        end
    end

    ui.drawFooter("ESC: Cancel", pad_key(gp.PAD_A) .. ": Download")
    display.flush()
end

local function draw_downloading()
    display.clear(BLACK)
    ui.drawHeader("System Update")

    draw_centered(120, "Downloading firmware...", WHITE, BLACK)

    local progress = 0
    if download_total > 0 then
        progress = download_received / download_total
    end
    ui.drawProgress(40, 150, 240, 16, progress, GREEN)

    local pct = math.floor(progress * 100)
    local kb_done = math.floor(download_received / 1024)
    local kb_total = math.floor(download_total / 1024)
    local status = string.format("%d%%  (%dK / %dK)", pct, kb_done, kb_total)
    draw_centered(174, status, WHITE, BLACK)

    ui.drawFooter("ESC: Cancel", "")
    display.flush()
end

local function draw_done()
    display.clear(BLACK)
    ui.drawHeader("System Update")

    draw_centered(110, "Download complete!", GREEN, BLACK)
    draw_centered(130, "Ready to install.", WHITE, BLACK)

    draw_centered(170, "The device will reboot", YELLOW, BLACK)
    draw_centered(186, "to apply the update.", YELLOW, BLACK)

    ui.drawFooter("ESC: Cancel", pad_key(gp.PAD_A) .. ": Install Now")
    display.flush()
end

local function draw_confirm()
    display.clear(BLACK)
    ui.drawHeader("System Update")

    draw_centered(110, "Apply firmware update?", YELLOW, BLACK)
    draw_centered(130, "Version " .. (remote_version or "?"), WHITE, BLACK)

    draw_centered(170, "The device will reboot.", GRAY, BLACK)
    draw_centered(186, "Do not power off during", GRAY, BLACK)
    draw_centered(202, "the update process.", GRAY, BLACK)

    draw_centered(240, pad_key(gp.PAD_A) .. ": Yes, Install", GREEN, BLACK)
    draw_centered(260, "ESC: No, Cancel", RED, BLACK)
    ui.drawFooter("ESC: No", pad_key(gp.PAD_A) .. ": Install")
    display.flush()
end

-- ── Main loop ───────────────────────────────────────────────────────────────

local last_screen = nil
local frame = 0

while true do
    input.update()
    local nav = gp.getNavPressed()  -- keys, rebound keys or a controller

    -- B or Esc exits or goes back
    if nav & gp.PAD_B ~= 0 then
        if current_conn then
            current_conn:close()
            current_conn = nil
        end
        if download_file then
            fs.close(download_file)
            download_file = nil
        end

        if current_screen == SCR_MAIN then
            return -- exit app
        else
            current_screen = SCR_MAIN
        end
    end

    -- A or Enter: the action of each screen
    if nav & gp.PAD_A ~= 0 then
        if current_screen == SCR_MAIN then
            check_for_update()
        elseif current_screen == SCR_AVAILABLE then
            start_download()
        elseif current_screen == SCR_DONE then
            current_screen = SCR_CONFIRM
        elseif current_screen == SCR_CONFIRM then
            -- Apply the update (this reboots on success)
            local ok, err = sys.applyUpdate(BIN_PATH)
            if not ok then
                error_msg = "Failed to apply update: " .. (err or "unknown error")
                ui.toast(error_msg, ui.TOAST_ERROR)
                current_screen = SCR_MAIN
            end
        elseif current_screen == SCR_UP_TO_DATE then
            start_download()
        end
    end

    -- Redraw on screen change or periodically during downloads
    local needs_draw = (current_screen ~= last_screen)
    if current_screen == SCR_DOWNLOADING then needs_draw = true end
    if current_screen == SCR_CHECKING then
        frame = frame + 1
        needs_draw = true
    end

    -- Handle download retry (checked each frame during SCR_DOWNLOADING)
    if download_retry_needed and current_screen == SCR_DOWNLOADING then
        download_retry_needed = false
        download_retries = download_retries + 1
        if download_retries <= MAX_DOWNLOAD_RETRIES then
            print("[UPDATER] Retrying download, attempt " .. download_retries .. "/" .. MAX_DOWNLOAD_RETRIES)
            download_received = 0
            download_total = remote_size
            download_file = nil
            if fs.exists(BIN_PATH) then
                fs.delete(BIN_PATH)
            end
            fetch_firmware(download_retry_url or remote_bin_url)
        else
            error_msg = "Download failed after " .. MAX_DOWNLOAD_RETRIES .. " attempts"
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_MAIN
        end
    end

    if needs_draw then
        if current_screen == SCR_MAIN then draw_main()
        elseif current_screen == SCR_CHECKING then draw_checking()
        elseif current_screen == SCR_AVAILABLE then draw_available()
        elseif current_screen == SCR_DOWNLOADING then draw_downloading()
        elseif current_screen == SCR_DONE then draw_done()
        elseif current_screen == SCR_UP_TO_DATE then draw_up_to_date()
        elseif current_screen == SCR_CONFIRM then draw_confirm()
        end
        last_screen = current_screen
    end

    sys.sleep(33) -- ~30 FPS
end
