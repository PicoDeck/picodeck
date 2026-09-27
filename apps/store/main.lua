-- PicoDeck App Store
-- Browse, install, update, and remove apps + firmware updates
-- Merges and replaces the old System Updater app

local display = picocalc.display
local input = picocalc.input
local sys = picocalc.sys
local net = picocalc.network
local fs = picocalc.fs
local ui = picocalc.ui
local zip = picocalc.zip
local crypto = picocalc.crypto

-- ── Configuration ──────────────────────────────────────────────────────────

local CATALOG_HOST = "store.picodeck.net"
local CATALOG_PORT = 443
local CATALOG_SSL  = true
local CATALOG_PATH = "/catalog.json"
local CACHE_DIR    = "/data/net.picodeck.store"
local CACHE_FILE   = CACHE_DIR .. "/catalog.json"
local STAGING_DIR  = "/apps/.staging"
local MAX_REDIRECTS = 3
local MAX_RETRIES   = 3

-- Firmware update paths (from old updater)
local BIN_PATH  = "/system/update.bin"
local HASH_PATH = "/system/update.sha256"
local SIG_PATH  = "/system/update.sig"

-- ── Screen dimensions and colors ───────────────────────────────────────────

local W, H = display.getWidth(), display.getHeight()

local BLACK  = 0x0000
local WHITE  = 0xFFFF
local RED    = 0xF800
local GREEN  = 0x07E0
local YELLOW = 0xFFE0
local CYAN   = 0x07FF
local GRAY   = 0x8410
local DKGRAY = 0x4208
local BLUE   = 0x001F
local ORANGE = 0xFBE0

-- ── Screen states ──────────────────────────────────────────────────────────

local SCR_LOADING    = 1
local SCR_BROWSE     = 2
local SCR_UPDATES    = 3
local SCR_INSTALLED  = 4
local SCR_DETAIL     = 5
local SCR_DOWNLOADING = 6
local SCR_FW_CONFIRM = 7
local SCR_NO_WIFI    = 8

local current_screen = SCR_LOADING
local last_screen = nil
local frame = 0
local error_msg = ""
local current_conn = nil

-- ── Catalog and app data ───────────────────────────────────────────────────

local catalog = nil         -- parsed catalog table
local catalog_apps = {}     -- array of catalog app entries
local installed_apps = {}   -- map of app_id → {version, path, dirname}
local update_list = {}      -- apps with available updates
local filtered_apps = {}    -- currently displayed list (after category filter)

-- Category filter
local CATEGORIES = {"All", "Games", "Tools", "System", "Demos", "Emulators", "Network"}
local category_idx = 1

-- Tab state
local TAB_BROWSE = 1
local TAB_UPDATES = 2
local TAB_INSTALLED = 3
local current_tab = TAB_BROWSE
local scroll_pos = 0
local selected_idx = 1

-- Detail screen state
local detail_app = nil

-- Download state
local download_stage = nil  -- "catalog", "app", "firmware"
local download_received = 0
local download_total = 0
local download_complete = false
local download_conn_id = 0
local download_retries = 0
local download_retry_needed = false
local download_app = nil    -- app being downloaded

-- Firmware update state (merged from updater)
local fw_info = nil         -- firmware info from catalog
local fw_bin_url = nil
local fw_hash_url = nil

-- ── Helpers (reused from updater) ──────────────────────────────────────────

local function draw_centered(y, text, fg, bg)
    local tw = display.textWidth(text)
    display.drawText(math.floor((W - tw) / 2), y, text, fg, bg or BLACK)
end

local function wrap_text(text, max_width)
    if not text or #text == 0 then return function() end end
    local lines = {}
    local line = ""
    for word in text:gmatch("%S+") do
        local test = (line == "") and word or (line .. " " .. word)
        if display.textWidth(test) > max_width then
            if line ~= "" then lines[#lines + 1] = line end
            line = word
        else
            line = test
        end
    end
    if line ~= "" then lines[#lines + 1] = line end
    local i = 0
    return function() i = i + 1; return lines[i] end
end

local function version_newer(remote, local_ver)
    local function parse(v)
        local parts = {}
        for n in v:gmatch("(%d+)") do parts[#parts + 1] = tonumber(n) end
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

local function json_get(json, key)
    local val = json:match('"' .. key .. '"%s*:%s*"([^"]*)"')
    if val then return val end
    val = json:match('"' .. key .. '"%s*:%s*(%d+)')
    if val then return val end
    return nil
end

local function json_get_bool(json, key)
    local val = json:match('"' .. key .. '"%s*:%s*(true)')
    if val then return true end
    val = json:match('"' .. key .. '"%s*:%s*(false)')
    if val then return false end
    return nil
end

local function parse_url(url)
    local host, path
    local ssl = false
    local port = 80

    if url:match("^https://") then
        ssl = true; port = 443
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

    local h, p = host:match("^(.+):(%d+)$")
    if h then host = h; port = tonumber(p) end
    return host, port, ssl, path
end

-- Developers can point a device at a staging indexer without reflashing:
-- picocalc.sysconfig.set("store_url", "https://picodeck-staging.example.workers.dev/catalog.json")
local function catalog_endpoint()
    local override = picocalc.sysconfig.get("store_url")
    if override and #override > 0 then
        local host, port, ssl, path = parse_url(override)
        if host then return host, port, ssl, path end
        print("[STORE] Ignoring invalid store_url: " .. override)
    end
    return CATALOG_HOST, CATALOG_PORT, CATALOG_SSL, CATALOG_PATH
end

-- Parse a JSON string array: "key": ["a", "b", "c"]
local function parse_string_array(json, key)
    local result = {}
    local start = json:find('"' .. key .. '"%s*:%s*%[')
    if not start then return result end
    local arr = json:match('%[(.-)%]', start)
    if arr then
        for val in arr:gmatch('"([^"]*)"') do
            result[#result + 1] = val
        end
    end
    return result
end

local function format_size(kb)
    if kb >= 1024 then
        return string.format("%.1f MB", kb / 1024)
    end
    return kb .. " KB"
end

local function truncate(text, max_w)
    if not text then return "" end
    if display.textWidth(text) <= max_w then return text end
    while #text > 0 and display.textWidth(text .. "...") > max_w do
        text = text:sub(1, -2)
    end
    return text .. "..."
end

-- ── Catalog parsing ────────────────────────────────────────────────────────

local function parse_catalog_json(json)
    local cat = {
        catalog_version = tonumber(json_get(json, "catalog_version")) or 1,
        apps = {},
        firmware = nil,
    }

    -- Parse firmware section
    local fw_start = json:find('"firmware"%s*:%s*{')
    if fw_start then
        local fw_end = json:find('}', fw_start)
        if fw_end then
            local fw_block = json:sub(fw_start, fw_end)
            cat.firmware = {
                version = json_get(fw_block, "version"),
                repo = json_get(fw_block, "repo"),
                release_tag = json_get(fw_block, "release_tag"),
                changelog = json_get(fw_block, "changelog"),
                size_kb = tonumber(json_get(fw_block, "size_kb")) or 0,
            }
        end
    end

    -- Parse apps array — find each { } block within "apps": [...]
    local apps_start = json:find('"apps"%s*:%s*%[')
    if not apps_start then return cat end

    local pos = json:find('%[', apps_start) + 1
    while true do
        local obj_start = json:find('{', pos)
        if not obj_start then break end

        -- Find matching closing brace
        local depth = 0
        local obj_end = obj_start
        for i = obj_start, #json do
            local c = json:sub(i, i)
            if c == '{' then depth = depth + 1
            elseif c == '}' then
                depth = depth - 1
                if depth == 0 then obj_end = i; break end
            end
        end

        local block = json:sub(obj_start, obj_end)
        local app = {
            id = json_get(block, "id"),
            dirname = json_get(block, "dirname"),
            name = json_get(block, "name"),
            description = json_get(block, "description"),
            long_description = json_get(block, "long_description"),
            version = json_get(block, "version") or "0.0.0",
            author = json_get(block, "author"),
            category = json_get(block, "category") or "demos",
            app_type = json_get(block, "app_type") or "lua",
            min_firmware = json_get(block, "min_firmware") or "0.0.0",
            size_kb = tonumber(json_get(block, "size_kb")) or 0,
            repo = json_get(block, "repo"),
            release_tag = json_get(block, "release_tag"),
            asset = json_get(block, "asset"),
            sha256 = json_get(block, "sha256"),
            homepage = json_get(block, "homepage"),
            removable = json_get_bool(block, "removable"),
            requirements = parse_string_array(block, "requirements"),
            stars = tonumber(json_get(block, "stars")) or 0,
        }
        if app.removable == nil then app.removable = true end
        if app.id then
            cat.apps[#cat.apps + 1] = app
        end

        pos = obj_end + 1
    end

    return cat
end

-- ── Local app scanning ─────────────────────────────────────────────────────

local function scan_installed()
    installed_apps = {}
    local entries = fs.listDir("/apps")
    if not entries then return end

    for _, entry in ipairs(entries) do
        if entry.is_dir then
            local json_path = "/apps/" .. entry.name .. "/app.json"
            if fs.exists(json_path) then
                local data = fs.readFile(json_path)
                if data then
                    local id = json_get(data, "id") or ("local." .. entry.name)
                    local version = json_get(data, "version") or "0.0.0"
                    installed_apps[id] = {
                        version = version,
                        dirname = entry.name,
                        path = "/apps/" .. entry.name,
                    }
                end
            end
        end
    end
end

-- Check which catalog apps have updates available
local function compute_updates()
    update_list = {}
    if not catalog then return end

    -- Check firmware update
    if catalog.firmware and catalog.firmware.version then
        local local_ver = sys.getVersion()
        if version_newer(catalog.firmware.version, local_ver) then
            fw_info = catalog.firmware
        else
            fw_info = nil
        end
    end

    -- Check app updates
    for _, app in ipairs(catalog.apps) do
        local inst = installed_apps[app.id]
        if inst and version_newer(app.version, inst.version) then
            update_list[#update_list + 1] = app
        end
    end
end

-- Apply category filter and build filtered list
local function apply_filter()
    filtered_apps = {}
    local cat_name = CATEGORIES[category_idx]:lower()

    for _, app in ipairs(catalog_apps) do
        if cat_name == "all" or app.category == cat_name then
            filtered_apps[#filtered_apps + 1] = app
        end
    end
end

-- ── Cleanup staging directories ────────────────────────────────────────────

local function cleanup_staging()
    local entries = fs.listDir("/apps")
    if not entries then return end
    for _, entry in ipairs(entries) do
        if entry.is_dir and entry.name:match("^%.staging") then
            print("[STORE] Cleaning up staging dir: /apps/" .. entry.name)
            fs.deleteRecursive("/apps/" .. entry.name)
        end
    end
end

-- ── Network: Fetch catalog ─────────────────────────────────────────────────

local function fetch_catalog()
    current_screen = SCR_LOADING
    download_stage = "catalog"

    local status = net.getStatus()
    if status ~= net.kStatusConnected then
        current_screen = SCR_NO_WIFI
        return
    end

    local host, port, ssl, path = catalog_endpoint()
    local conn = net.http.new(host, port, ssl)
    if not conn then
        error_msg = "Cannot connect to catalog server"
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_BROWSE
        return
    end

    conn:setConnectTimeout(15)
    conn:setReadTimeout(30)
    -- Large ring (PSRAM): nothing slows the sender on the device; 32 KB
    -- if the heap cannot spare it.
    if not conn:setReadBufferSize(256 * 1024) then
        conn:setReadBufferSize(32 * 1024)
    end
    current_conn = conn

    local body = ""

    conn:setRequestCallback(function()
        while conn:getBytesAvailable() > 0 do
            local data = conn:read()
            if data then body = body .. data end
        end
    end)

    conn:setRequestCompleteCallback(function()
        local http_status = conn:getResponseStatus()
        conn:close()
        current_conn = nil

        if http_status ~= 200 then
            error_msg = "Catalog fetch failed: HTTP " .. tostring(http_status)
            ui.toast(error_msg, ui.TOAST_ERROR)
            -- Try cached catalog
            if fs.exists(CACHE_FILE) then
                body = fs.readFile(CACHE_FILE) or ""
            end
            if #body == 0 then
                current_screen = SCR_BROWSE
                return
            end
        end

        -- Cache the catalog
        fs.mkdir(CACHE_DIR)
        local cf = fs.open(CACHE_FILE, "w")
        if cf then
            fs.write(cf, body)
            fs.close(cf)
        end

        catalog = parse_catalog_json(body)
        catalog_apps = catalog.apps
        scan_installed()
        compute_updates()
        apply_filter()

        current_screen = SCR_BROWSE
        download_stage = nil
    end)

    conn:setConnectionClosedCallback(function()
        current_conn = nil
        if download_stage == "catalog" then
            -- Try cached
            if fs.exists(CACHE_FILE) then
                local data = fs.readFile(CACHE_FILE)
                if data and #data > 0 then
                    catalog = parse_catalog_json(data)
                    catalog_apps = catalog.apps
                    scan_installed()
                    compute_updates()
                    apply_filter()
                end
            end
            current_screen = SCR_BROWSE
            download_stage = nil
        end
    end)

    conn:get(path, {["User-Agent"] = "PicoDeck-Store/1.1"})
end

-- ── Network: Download straight to a file ───────────────────────────────────
-- Streams the response body to dest_path as it arrives (32KB read buffer, SD
-- in slow mode while the radio is active) — peak memory is one read buffer
-- instead of the whole download in PSRAM. on_complete(ok, err); on any
-- failure the partial file is deleted.

local function download_file_with_redirects(url, dest_path, redirect_count, on_complete)
    redirect_count = redirect_count or 0
    if redirect_count >= MAX_REDIRECTS then
        on_complete(false, "Too many redirects")
        return
    end

    local host, port, ssl, path = parse_url(url)
    if not host then
        on_complete(false, "Invalid URL")
        return
    end

    local conn = net.http.new(host, port, ssl)
    if not conn then
        on_complete(false, "Cannot connect")
        return
    end

    conn:setConnectTimeout(30)
    conn:setReadTimeout(60)
    -- Large ring (PSRAM): nothing slows the sender on the device; 32 KB
    -- if the heap cannot spare it.
    if not conn:setReadBufferSize(256 * 1024) then
        conn:setReadBufferSize(32 * 1024)
    end
    current_conn = conn
    download_complete = false

    download_conn_id = download_conn_id + 1
    local my_conn_id = download_conn_id

    local wf = nil          -- staging file handle, opened on first data
    local write_failed = nil

    local function close_and_delete()
        if wf then fs.close(wf); wf = nil end
        fs.setSlowMode(false)
        fs.delete(dest_path)
    end

    -- Write everything currently buffered on the connection to dest_path.
    local function drain_to_file()
        if write_failed then return end
        if not wf then
            if not fs.ensureReady() then
                write_failed = "SD card not responding"
                return
            end
            fs.setSlowMode(true) -- radio active: SPI0 must run at 1 MHz
            wf = fs.open(dest_path, "w")
            if not wf then
                fs.setSlowMode(false)
                write_failed = "Cannot write " .. dest_path
                return
            end
        end
        while true do
            local avail = conn:getBytesAvailable()
            if avail <= 0 then break end
            local data = conn:read()
            if not data or #data == 0 then break end
            if download_total > 0 then
                local remaining = download_total - download_received
                if remaining <= 0 then break end
                if #data > remaining then data = data:sub(1, remaining) end
            end
            fs.write(wf, data)
            download_received = download_received + #data
        end
    end

    conn:setHeadersReadCallback(function()
        if my_conn_id ~= download_conn_id then return end
        local hdrs = conn:getResponseHeaders()
        if hdrs and hdrs["content-length"] then
            local cl = tonumber(hdrs["content-length"])
            if cl and cl > 0 then download_total = cl end
        end
    end)

    conn:setRequestCallback(function()
        if my_conn_id ~= download_conn_id then return end
        if download_complete then return end
        local status = conn:getResponseStatus()
        if status and status ~= 200 then return end

        drain_to_file()
    end)

    conn:setRequestCompleteCallback(function()
        if my_conn_id ~= download_conn_id then return end
        local http_status = conn:getResponseStatus()
        local conn_err = conn:getError()

        -- Handle redirects
        if http_status and http_status >= 300 and http_status < 400 then
            local resp_headers = conn:getResponseHeaders()
            local location = resp_headers and resp_headers["location"]
            conn:close()
            if current_conn == conn then current_conn = nil end
            close_and_delete()
            if location then
                download_file_with_redirects(location, dest_path, redirect_count + 1, on_complete)
            else
                on_complete(false, "Redirect without location")
            end
            return
        end

        if conn_err then
            conn:close()
            if current_conn == conn then current_conn = nil end
            close_and_delete()
            on_complete(false, conn_err)
            return
        end

        -- Drain remaining
        drain_to_file()

        if http_status and http_status ~= 200 then
            conn:close()
            if current_conn == conn then current_conn = nil end
            close_and_delete()
            on_complete(false, "HTTP " .. tostring(http_status))
            return
        end

        -- Transfer done; everything is already on disk.
        if wf then fs.close(wf); wf = nil end
        fs.setSlowMode(false)
        conn:close()
        if current_conn == conn then current_conn = nil end

        if write_failed then
            fs.delete(dest_path)
            on_complete(false, write_failed)
            return
        end
        sys.sleep(100)

        -- Voltage-safe verify/extract window: on_complete hashes and unpacks
        -- on the SD card, so disconnect WiFi, pause Core 1 and slow SPI for
        -- its duration (same brown-out posture as the old buffered flow).
        picocalc.wifi.disconnect()
        local hw_start = sys.getTimeMs()
        while not picocalc.network.isHwDisconnected() do
            sys.sleep(10)
            if sys.getTimeMs() - hw_start > 2000 then break end
        end

        local paused = sys.pauseBackground()
        fs.setSlowMode(true)

        if not fs.ensureReady() then
            fs.setSlowMode(false)
            sys.resumeBackground()
            on_complete(false, "SD card not responding")
            return
        end

        on_complete(true, nil)

        fs.setSlowMode(false)
        sys.resumeBackground()
    end)

    conn:setConnectionClosedCallback(function()
        if my_conn_id ~= download_conn_id then return end
        if current_conn == conn then current_conn = nil end
        if download_complete then return end
        if download_stage then
            close_and_delete()
            on_complete(false, "Connection closed")
        end
    end)

    conn:get(path, {["User-Agent"] = "PicoDeck-Store/1.0"})
end

-- Fetch a small text body (a checksum file) into memory, following
-- redirects: on_done(body) or on_done(nil, err).  Leaves WiFi up, unlike
-- download_file_with_redirects, so a download can follow it.
local function fetch_small_text(url, redirect_count, on_done)
    if redirect_count >= MAX_REDIRECTS then
        on_done(nil, "Too many redirects")
        return
    end
    local host, port, ssl, path = parse_url(url)
    if not host then on_done(nil, "Invalid URL") return end
    local conn = net.http.new(host, port, ssl)
    if not conn then on_done(nil, "Cannot connect") return end
    conn:setConnectTimeout(15)
    conn:setReadTimeout(15)
    conn:setReadBufferSize(512)
    current_conn = conn

    local body, finished = "", false
    local function drain()
        while conn:getBytesAvailable() > 0 do
            local data = conn:read()
            if not data or #data == 0 then break end
            if #body < 1024 then body = body .. data end
        end
    end
    local function finish(text, err)
        if finished then return end
        finished = true
        conn:close()
        if current_conn == conn then current_conn = nil end
        on_done(text, err)
    end

    conn:setRequestCallback(drain)
    conn:setRequestCompleteCallback(function()
        drain()
        local status = conn:getResponseStatus()
        local hdrs = conn:getResponseHeaders()
        if status and status >= 300 and status < 400 and hdrs and hdrs["location"] then
            finished = true
            conn:close()
            if current_conn == conn then current_conn = nil end
            fetch_small_text(hdrs["location"], redirect_count + 1, on_done)
            return
        end
        if status ~= 200 then
            finish(nil, "HTTP " .. tostring(status))
            return
        end
        finish(body)
    end)
    conn:setConnectionClosedCallback(function()
        finish(nil, conn:getError() or "Connection closed")
    end)
    conn:get(path, {["User-Agent"] = "PicoDeck-Store/1.1"})
end

-- ── App installation ───────────────────────────────────────────────────────

local function install_app(app)
    if not app or not app.repo or not app.release_tag or not app.asset then
        error_msg = "Missing download information"
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_DETAIL
        return
    end

    -- Self-update guard: the store can't safely replace itself while running
    if app.id == "net.picodeck.store" then
        error_msg = "Store updates are applied via firmware update"
        ui.toast(error_msg, ui.TOAST_INFO)
        current_screen = SCR_DETAIL
        return
    end

    -- Check disk space
    local disk = fs.diskInfo()
    if disk and app.size_kb > 0 and disk.free < app.size_kb + 64 then
        error_msg = "Not enough space (" .. format_size(disk.free) .. " free)"
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_DETAIL
        return
    end

    -- Check firmware compatibility
    if app.min_firmware and version_newer(app.min_firmware, sys.getVersion()) then
        error_msg = "Requires firmware " .. app.min_firmware .. "+"
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_DETAIL
        return
    end

    -- Integrity: refuse to install anything the catalog doesn't checksum.
    -- (Catalog generator must publish sha256 for every asset.)
    if type(app.sha256) ~= "string" or #app.sha256 ~= 64 then
        error_msg = "No checksum in catalog — install refused"
        ui.toast(error_msg, ui.TOAST_ERROR)
        current_screen = SCR_DETAIL
        return
    end

    current_screen = SCR_DOWNLOADING
    download_stage = "app"
    download_app = app
    download_received = 0
    download_total = (app.size_kb or 0) * 1024
    download_retries = 0
    download_retry_needed = false

    local url = "https://github.com/" .. app.repo ..
                "/releases/download/" .. app.release_tag .. "/" .. app.asset

    local zip_path = CACHE_DIR .. "/" .. app.id .. ".zip"
    fs.mkdir(CACHE_DIR)

    local function do_download()
        download_file_with_redirects(url, zip_path, 0, function(ok, err)
            if not ok then
                print("[STORE] Download failed: " .. tostring(err))
                download_retry_needed = true
                return
            end

            -- Verify SHA-256 (mandatory — hashless entries are refused
            -- before the download starts). Streamed hash, no whole-file
            -- read-back. crypto is absent in the simulator only.
            if crypto and crypto.sha256File then
                local hash = crypto.sha256File(zip_path)
                if not hash or hash:lower() ~= app.sha256:lower() then
                    print("[STORE] SHA-256 mismatch!")
                    fs.delete(zip_path)
                    download_retry_needed = true
                    return
                end
            else
                print("[STORE] WARNING: crypto unavailable (simulator?) — skipping checksum verification")
            end

            -- Extract ZIP
            local dirname = app.dirname or app.id:match("[^%.]+$") or app.id
            local staging_path = "/apps/.staging_" .. dirname
            fs.deleteRecursive(staging_path)
            fs.mkdir(staging_path)

            local ok, extract_err = zip.extract(zip_path, staging_path)
            fs.delete(zip_path)  -- clean up ZIP

            if not ok then
                fs.deleteRecursive(staging_path)
                error_msg = "Extract failed: " .. tostring(extract_err)
                ui.toast(error_msg, ui.TOAST_ERROR)
                current_screen = SCR_DETAIL
                return
            end

            -- If updating, remove old app directory
            local final_path = "/apps/" .. dirname
            if fs.exists(final_path) then
                fs.deleteRecursive(final_path)
            end

            -- Move staging to final location
            local rename_ok, rename_err = fs.rename(staging_path, final_path)
            if not rename_ok then
                error_msg = "Install failed: " .. tostring(rename_err)
                ui.toast(error_msg, ui.TOAST_ERROR)
                fs.deleteRecursive(staging_path)
                current_screen = SCR_DETAIL
                return
            end

            -- Success!
            download_complete = true
            download_stage = nil
            scan_installed()
            compute_updates()
            apply_filter()
            ui.toast(app.name .. " installed!", ui.TOAST_SUCCESS)
            current_screen = SCR_DETAIL
        end)
    end

    do_download()
end

-- ── App removal ────────────────────────────────────────────────────────────

local function remove_app(app)
    if not app then return end
    local dirname = app.dirname or app.id:match("[^%.]+$") or app.id
    local app_path = "/apps/" .. dirname
    local data_path = "/data/" .. app.id

    local msg = "Remove " .. (app.name or dirname) .. "?"
    if fs.exists(data_path) then
        msg = msg .. "\nApp data will also be deleted."
    end

    if not ui.confirm(msg) then return end

    if fs.exists(app_path) then
        fs.deleteRecursive(app_path)
    end
    if fs.exists(data_path) then
        fs.deleteRecursive(data_path)
    end

    scan_installed()
    compute_updates()
    apply_filter()
    ui.toast((app.name or dirname) .. " removed", ui.TOAST_INFO)
    current_screen = SCR_BROWSE
end

-- ── Firmware update (merged from updater) ──────────────────────────────────

local function start_firmware_update()
    if not fw_info then return end

    current_screen = SCR_DOWNLOADING
    download_stage = "firmware"
    download_received = 0
    download_total = (fw_info.size_kb or 0) * 1024
    download_retries = 0
    download_retry_needed = false

    local tag = fw_info.release_tag
    local repo = fw_info.repo or "PicoDeck/picodeck"
    local url = "https://github.com/" .. repo ..
                "/releases/download/" .. tag .. "/picodeck.bin"
    fw_hash_url = "https://github.com/" .. repo ..
                  "/releases/download/" .. tag .. "/picodeck.sha256"
    local fw_sig_url = "https://github.com/" .. repo ..
                       "/releases/download/" .. tag .. "/picodeck.sig"

    if fs.exists(BIN_PATH) then fs.delete(BIN_PATH) end
    if fs.exists(HASH_PATH) then fs.delete(HASH_PATH) end
    if fs.exists(SIG_PATH) then fs.delete(SIG_PATH) end

    -- The OS refuses to flash an image without /system/update.sha256 and a
    -- valid /system/update.sig (ECDSA signature by the PicoDeck update key), so
    -- fetch both (small, in memory) before the image; a release missing
    -- either is not offered for install.
    fetch_small_text(fw_hash_url, 0, function(body, herr)
        local hash = body and body:match("^%s*(%x+)%s*$")
        if not hash or #hash ~= 64 then
            print("[STORE] Firmware checksum download failed: " ..
                  tostring(herr or "no SHA-256 in response"))
            download_retry_needed = true
            return
        end
      fetch_small_text(fw_sig_url, 0, function(sig, serr)
        -- DER ECDSA P-256 signature: 8..72 bytes, starts with SEQUENCE.
        if not sig or #sig < 8 or #sig > 128 or sig:byte(1) ~= 0x30 then
            print("[STORE] Firmware signature download failed: " ..
                  tostring(serr or "no signature in response"))
            download_retry_needed = true
            return
        end

        download_file_with_redirects(url, BIN_PATH, 0, function(ok, err)
            if not ok then
                print("[STORE] Firmware download failed: " .. tostring(err))
                download_retry_needed = true
                return
            end

            -- Verify size
            local actual = fs.size(BIN_PATH)
            if actual and actual < 256 then
                fs.delete(BIN_PATH)
                download_retry_needed = true
                return
            end

            local hf = fs.open(HASH_PATH, "w")
            if not hf then
                fs.delete(BIN_PATH)
                download_retry_needed = true
                return
            end
            fs.write(hf, hash:lower() .. "\n")
            fs.close(hf)

            local sf = fs.open(SIG_PATH, "w")
            if not sf then
                fs.delete(BIN_PATH)
                fs.delete(HASH_PATH)
                download_retry_needed = true
                return
            end
            fs.write(sf, sig)
            fs.close(sf)

            download_complete = true
            download_stage = nil
            current_screen = SCR_FW_CONFIRM
        end)
      end)
    end)
end

-- ── Drawing helpers ────────────────────────────────────────────────────────

-- Header rows: drawHeader returns them (older firmware returns nothing: 29).
local function draw_header(title)
    return ui.drawHeader(title) or 29
end

-- Draws the tabs below a header of `top` rows; returns the first free row.
local function draw_tab_bar(top)
    local tab_labels = {"Browse", "Updates", "Installed"}
    if #update_list > 0 or fw_info then
        local count = #update_list + (fw_info and 1 or 0)
        tab_labels[2] = "Updates(" .. count .. ")"
    end
    local _, tab_h = ui.drawTabs(top, tab_labels, current_tab)
    return top + tab_h
end

local function get_app_status(app)
    local inst = installed_apps[app.id]
    if not inst then return "new", nil end
    if version_newer(app.version, inst.version) then
        return "update", inst.version
    end
    return "installed", inst.version
end

local function draw_app_list_item(y, app, is_selected)
    local bg = is_selected and DKGRAY or BLACK
    if is_selected then
        display.fillRect(0, y - 2, W, 28, bg)
    end

    local status, inst_ver = get_app_status(app)

    -- App name
    local name_color = WHITE
    if status == "update" then name_color = CYAN end
    local name = truncate(app.name or "?", W - 100)
    display.drawText(8, y, name, name_color, bg)

    -- Version on right
    local ver_text = "v" .. app.version
    local ver_w = display.textWidth(ver_text)
    display.drawText(W - ver_w - 8, y, ver_text, GRAY, bg)

    -- Status badge
    if status == "new" then
        -- no badge
    elseif status == "update" then
        display.drawText(8, y + 13, "[UPDATE]", CYAN, bg)
    elseif status == "installed" then
        display.drawText(8, y + 13, "[INSTALLED]", GREEN, bg)
    end

    -- Description (truncated)
    local desc_x = 8
    if status ~= "new" then desc_x = desc_x + display.textWidth("[INSTALLED] ") end
    local desc = truncate(app.description or "", W - desc_x - 8)
    display.drawText(desc_x, y + 13, desc, GRAY, bg)
end

-- ── Screen drawing ─────────────────────────────────────────────────────────

local function draw_loading()
    display.clear(BLACK)
    ui.drawHeader("App Store")
    ui.drawSpinner(W / 2, 140, 12, frame)
    draw_centered(165, "Loading catalog...", WHITE, BLACK)
    display.flush()
end

local function draw_no_wifi()
    display.clear(BLACK)
    ui.drawHeader("App Store")
    draw_centered(120, "WiFi not connected", RED, BLACK)
    draw_centered(145, "Configure WiFi credentials", GRAY, BLACK)
    draw_centered(160, "in system settings first.", GRAY, BLACK)
    draw_centered(195, "Enter: Retry", CYAN, BLACK)
    ui.drawFooter("ESC: Exit", "Enter: Retry")
    display.flush()
end

local function draw_browse()
    display.clear(BLACK)
    local tab_h = draw_tab_bar(draw_header("App Store"))

    -- Category filter bar
    local cat_y = tab_h + 2
    local cat_text = "< " .. CATEGORIES[category_idx] .. " >"
    local cat_w = display.textWidth(cat_text)
    display.drawText(math.floor((W - cat_w) / 2), cat_y, cat_text, YELLOW, BLACK)
    display.drawLine(0, cat_y + 14, W, cat_y + 14, DKGRAY)

    -- App list
    local list_y = cat_y + 18
    local item_h = 30
    local visible_count = math.floor((H - list_y - 24) / item_h)

    if #filtered_apps == 0 then
        draw_centered(list_y + 40, "No apps available", GRAY, BLACK)
    else
        -- Clamp selection and scroll
        if selected_idx < 1 then selected_idx = 1 end
        if selected_idx > #filtered_apps then selected_idx = #filtered_apps end
        if selected_idx < scroll_pos + 1 then scroll_pos = selected_idx - 1 end
        if selected_idx > scroll_pos + visible_count then scroll_pos = selected_idx - visible_count end

        for i = 1, visible_count do
            local idx = scroll_pos + i
            if idx > #filtered_apps then break end
            local app = filtered_apps[idx]
            local y = list_y + (i - 1) * item_h
            draw_app_list_item(y, app, idx == selected_idx)
        end

        -- Scroll indicator
        if #filtered_apps > visible_count then
            local bar_h = math.max(10, math.floor(visible_count / #filtered_apps * (H - list_y - 24)))
            local bar_y = list_y + math.floor(scroll_pos / (#filtered_apps - visible_count) * (H - list_y - 24 - bar_h))
            display.fillRect(W - 3, bar_y, 3, bar_h, GRAY)
        end
    end

    ui.drawFooter("ESC:Exit  \x1b\x1a:Category", "\x18\x19:Select  \x0d:Open")
    display.flush()
end

local function draw_updates()
    display.clear(BLACK)
    local list_y = draw_tab_bar(draw_header("App Store")) + 4
    local item_h = 30
    local visible_count = math.floor((H - list_y - 24) / item_h)
    local items = {}

    -- Firmware update at top
    if fw_info then
        items[#items + 1] = {
            is_firmware = true,
            name = "PicoDeck Firmware",
            version = fw_info.version,
            description = fw_info.changelog or "Firmware update available",
            size_kb = fw_info.size_kb,
        }
    end

    -- App updates
    for _, app in ipairs(update_list) do
        items[#items + 1] = app
    end

    if #items == 0 then
        draw_centered(list_y + 50, "Everything is up to date!", GREEN, BLACK)
    else
        if selected_idx < 1 then selected_idx = 1 end
        if selected_idx > #items then selected_idx = #items end

        for i = 1, math.min(#items, visible_count) do
            local idx = scroll_pos + i
            if idx > #items then break end
            local item = items[idx]
            local y = list_y + (i - 1) * item_h
            local is_sel = idx == selected_idx
            local bg = is_sel and DKGRAY or BLACK

            if is_sel then display.fillRect(0, y - 2, W, 28, bg) end

            if item.is_firmware then
                display.drawText(8, y, "PicoDeck Firmware", ORANGE, bg)
                local ver = sys.getVersion() .. " -> " .. fw_info.version
                display.drawText(8, y + 13, ver, CYAN, bg)
            else
                local inst = installed_apps[item.id]
                display.drawText(8, y, truncate(item.name, W - 80), CYAN, bg)
                local ver_text = (inst and inst.version or "?") .. " -> " .. item.version
                display.drawText(8, y + 13, ver_text, GRAY, bg)
            end
        end
    end

    ui.drawFooter("ESC:Exit", "\x18\x19:Select  \x0d:Update")
    display.flush()
end

local function draw_installed()
    display.clear(BLACK)
    local list_y = draw_tab_bar(draw_header("App Store")) + 4
    local item_h = 30
    local visible_count = math.floor((H - list_y - 24) / item_h)

    -- Build installed list from catalog (or raw scan)
    local items = {}
    for id, info in pairs(installed_apps) do
        -- Find catalog entry for richer info
        local cat_app = nil
        for _, a in ipairs(catalog_apps) do
            if a.id == id then cat_app = a; break end
        end
        items[#items + 1] = cat_app or {
            id = id,
            name = info.dirname,
            version = info.version,
            dirname = info.dirname,
            description = "",
        }
    end

    -- Sort by name
    table.sort(items, function(a, b) return (a.name or "") < (b.name or "") end)

    if #items == 0 then
        draw_centered(list_y + 50, "No apps installed", GRAY, BLACK)
    else
        if selected_idx < 1 then selected_idx = 1 end
        if selected_idx > #items then selected_idx = #items end

        for i = 1, math.min(#items, visible_count) do
            local idx = scroll_pos + i
            if idx > #items then break end
            local app = items[idx]
            local y = list_y + (i - 1) * item_h
            local is_sel = idx == selected_idx
            local bg = is_sel and DKGRAY or BLACK

            if is_sel then display.fillRect(0, y - 2, W, 28, bg) end
            display.drawText(8, y, truncate(app.name or "?", W - 80), WHITE, bg)
            local ver_w = display.textWidth("v" .. app.version)
            display.drawText(W - ver_w - 8, y, "v" .. app.version, GRAY, bg)
            display.drawText(8, y + 13, truncate(app.description or "", W - 16), GRAY, bg)
        end
    end

    ui.drawFooter("ESC:Exit  Del:Remove", "\x18\x19:Select  \x0d:Details")
    display.flush()
end

local function draw_detail()
    if not detail_app then current_screen = SCR_BROWSE; return end
    local app = detail_app

    display.clear(BLACK)
    local y = draw_header(truncate(app.name or "?", W - 100)) + 7

    -- App info
    display.drawText(8, y, "Author:", GRAY, BLACK)
    display.drawText(70, y, app.author or "Unknown", WHITE, BLACK)
    y = y + 16

    if app.stars and app.stars > 0 then
        display.drawText(8, y, "Stars:", GRAY, BLACK)
        display.drawText(70, y, tostring(app.stars) .. "  " .. (app.repo or ""), WHITE, BLACK)
        y = y + 16
    end

    display.drawText(8, y, "Category:", GRAY, BLACK)
    display.drawText(70, y, (app.category or "?"):sub(1,1):upper() .. (app.category or "?"):sub(2), WHITE, BLACK)
    y = y + 16

    display.drawText(8, y, "Size:", GRAY, BLACK)
    display.drawText(70, y, format_size(app.size_kb or 0), WHITE, BLACK)
    y = y + 16

    display.drawText(8, y, "Version:", GRAY, BLACK)
    display.drawText(70, y, app.version or "?", WHITE, BLACK)
    y = y + 16

    if app.requirements and #app.requirements > 0 then
        display.drawText(8, y, "Needs:", GRAY, BLACK)
        display.drawText(70, y, table.concat(app.requirements, ", "), WHITE, BLACK)
        y = y + 16
    end

    if app.min_firmware and app.min_firmware ~= "0.0.0" then
        local fw_ok = not version_newer(app.min_firmware, sys.getVersion())
        local fw_color = fw_ok and WHITE or RED
        display.drawText(8, y, "Firmware:", GRAY, BLACK)
        display.drawText(70, y, app.min_firmware .. "+", fw_color, BLACK)
        y = y + 16
    end

    -- Description
    y = y + 8
    display.drawLine(8, y, W - 8, y, DKGRAY)
    y = y + 6
    local desc = app.long_description or app.description or ""
    for line in wrap_text(desc, W - 16) do
        display.drawText(8, y, line, WHITE, BLACK)
        y = y + 13
        if y > H - 60 then break end
    end

    -- Action buttons
    local status, inst_ver = get_app_status(app)
    local btn_y = H - 44

    if status == "new" then
        local fw_ok = not app.min_firmware or not version_newer(app.min_firmware, sys.getVersion())
        if fw_ok then
            draw_centered(btn_y, "[ Enter: Install ]", GREEN, BLACK)
        else
            draw_centered(btn_y, "Requires firmware update", RED, BLACK)
        end
    elseif status == "update" then
        draw_centered(btn_y, "[ Enter: Update " .. inst_ver .. " -> " .. app.version .. " ]", CYAN, BLACK)
    elseif status == "installed" then
        draw_centered(btn_y, "Installed v" .. inst_ver, GREEN, BLACK)
        if app.removable ~= false then
            draw_centered(btn_y + 16, "Del: Remove", RED, BLACK)
        end
    end

    ui.drawFooter("ESC: Back", "")
    display.flush()
end

local function draw_downloading()
    display.clear(BLACK)
    ui.drawHeader("App Store")

    local label = "Downloading..."
    if download_stage == "firmware" then
        label = "Downloading firmware..."
    elseif download_app then
        label = "Installing " .. (download_app.name or "app") .. "..."
    end

    draw_centered(120, label, WHITE, BLACK)

    local progress = 0
    if download_total > 0 then
        progress = download_received / download_total
    end
    ui.drawProgress(40, 150, 240, 16, progress, GREEN)

    local pct = math.floor(progress * 100)
    local kb_done = math.floor(download_received / 1024)
    local kb_total = math.floor(download_total / 1024)
    draw_centered(174, string.format("%d%%  (%s / %s)", pct,
        format_size(kb_done), format_size(kb_total)), WHITE, BLACK)

    if download_retries > 0 then
        draw_centered(200, "Retry " .. download_retries .. "/" .. MAX_RETRIES, YELLOW, BLACK)
    end

    ui.drawFooter("ESC: Cancel", "")
    display.flush()
end

local function draw_fw_confirm()
    display.clear(BLACK)
    ui.drawHeader("Firmware Update")

    draw_centered(100, "Apply firmware update?", YELLOW, BLACK)
    if fw_info then
        draw_centered(120, sys.getVersion() .. " -> " .. fw_info.version, WHITE, BLACK)
    end

    draw_centered(160, "The device will reboot.", GRAY, BLACK)
    draw_centered(176, "Do not power off during", GRAY, BLACK)
    draw_centered(192, "the update process.", GRAY, BLACK)

    draw_centered(230, "Enter: Install & Reboot", GREEN, BLACK)
    draw_centered(250, "ESC: Cancel", RED, BLACK)
    ui.drawFooter("ESC: Cancel", "Enter: Install")
    display.flush()
end

-- ── Input handling ─────────────────────────────────────────────────────────

local function handle_input()
    input.update()
    local pressed = input.getButtonsPressed()
    if pressed == 0 then return end

    -- ESC: exit or go back
    if pressed & input.BTN_ESC ~= 0 then
        if current_conn then current_conn:close(); current_conn = nil end

        if current_screen == SCR_DETAIL then
            current_screen = current_tab == TAB_BROWSE and SCR_BROWSE
                          or current_tab == TAB_UPDATES and SCR_UPDATES
                          or SCR_INSTALLED
            return
        elseif current_screen == SCR_DOWNLOADING then
            download_stage = nil
            current_screen = SCR_BROWSE
            return
        elseif current_screen == SCR_FW_CONFIRM then
            current_screen = SCR_UPDATES
            return
        elseif current_screen == SCR_BROWSE or current_screen == SCR_UPDATES
               or current_screen == SCR_INSTALLED or current_screen == SCR_NO_WIFI then
            return true  -- exit app
        end
    end

    -- Tab switching: F1/F2/F3 or ,/.
    if current_screen == SCR_BROWSE or current_screen == SCR_UPDATES
       or current_screen == SCR_INSTALLED then
        local new_tab = nil
        if pressed & input.BTN_F1 ~= 0 then new_tab = TAB_BROWSE end
        if pressed & input.BTN_F2 ~= 0 then new_tab = TAB_UPDATES end
        if pressed & input.BTN_F3 ~= 0 then new_tab = TAB_INSTALLED end

        if new_tab and new_tab ~= current_tab then
            current_tab = new_tab
            selected_idx = 1
            scroll_pos = 0
            if new_tab == TAB_BROWSE then current_screen = SCR_BROWSE
            elseif new_tab == TAB_UPDATES then current_screen = SCR_UPDATES
            else current_screen = SCR_INSTALLED end
            return
        end
    end

    -- Navigation
    if pressed & input.BTN_UP ~= 0 then
        selected_idx = selected_idx - 1
        if selected_idx < 1 then selected_idx = 1 end
    end
    if pressed & input.BTN_DOWN ~= 0 then
        selected_idx = selected_idx + 1
    end

    -- Category filter (left/right in Browse tab)
    if current_screen == SCR_BROWSE then
        if pressed & input.BTN_LEFT ~= 0 then
            category_idx = category_idx - 1
            if category_idx < 1 then category_idx = #CATEGORIES end
            selected_idx = 1; scroll_pos = 0
            apply_filter()
        end
        if pressed & input.BTN_RIGHT ~= 0 then
            category_idx = (category_idx % #CATEGORIES) + 1
            selected_idx = 1; scroll_pos = 0
            apply_filter()
        end
    end

    -- Enter: select item
    if pressed & input.BTN_ENTER ~= 0 then
        if current_screen == SCR_NO_WIFI then
            fetch_catalog()
            return
        end

        if current_screen == SCR_BROWSE and #filtered_apps > 0 then
            if selected_idx >= 1 and selected_idx <= #filtered_apps then
                detail_app = filtered_apps[selected_idx]
                current_screen = SCR_DETAIL
            end
        elseif current_screen == SCR_UPDATES then
            -- Build combined list
            local items = {}
            if fw_info then items[#items + 1] = {is_firmware = true} end
            for _, a in ipairs(update_list) do items[#items + 1] = a end

            if selected_idx >= 1 and selected_idx <= #items then
                local item = items[selected_idx]
                if item.is_firmware then
                    start_firmware_update()
                else
                    install_app(item)
                end
            end
        elseif current_screen == SCR_INSTALLED then
            local items = {}
            for id, info in pairs(installed_apps) do
                local cat_app = nil
                for _, a in ipairs(catalog_apps) do
                    if a.id == id then cat_app = a; break end
                end
                items[#items + 1] = cat_app or {
                    id = id, name = info.dirname, version = info.version,
                    dirname = info.dirname, description = "",
                }
            end
            table.sort(items, function(a, b) return (a.name or "") < (b.name or "") end)

            if selected_idx >= 1 and selected_idx <= #items then
                detail_app = items[selected_idx]
                current_screen = SCR_DETAIL
            end
        elseif current_screen == SCR_DETAIL then
            if detail_app then
                local status = get_app_status(detail_app)
                if status == "new" or status == "update" then
                    install_app(detail_app)
                end
            end
        elseif current_screen == SCR_FW_CONFIRM then
            local ok, err = sys.applyUpdate(BIN_PATH)
            if not ok then
                error_msg = "Failed: " .. (err or "unknown")
                ui.toast(error_msg, ui.TOAST_ERROR)
                current_screen = SCR_UPDATES
            end
        end
    end

    -- Delete key for removal
    if pressed & input.BTN_DEL ~= 0 then
        if current_screen == SCR_DETAIL and detail_app then
            if detail_app.removable ~= false then
                local status = get_app_status(detail_app)
                if status == "installed" or status == "update" then
                    remove_app(detail_app)
                end
            end
        elseif current_screen == SCR_INSTALLED then
            local items = {}
            for id, info in pairs(installed_apps) do
                local cat_app = nil
                for _, a in ipairs(catalog_apps) do
                    if a.id == id then cat_app = a; break end
                end
                items[#items + 1] = cat_app or {
                    id = id, name = info.dirname, version = info.version,
                    dirname = info.dirname, description = "",
                }
            end
            table.sort(items, function(a, b) return (a.name or "") < (b.name or "") end)

            if selected_idx >= 1 and selected_idx <= #items then
                local app = items[selected_idx]
                if app.removable ~= false then
                    remove_app(app)
                end
            end
        end
    end

    return false
end

-- ── Main loop ──────────────────────────────────────────────────────────────

-- Initialize
fs.mkdir(CACHE_DIR)
cleanup_staging()
scan_installed()
fetch_catalog()

while true do
    local should_exit = handle_input()
    if should_exit then return end

    -- Handle download retry
    if download_retry_needed and current_screen == SCR_DOWNLOADING then
        download_retry_needed = false
        download_retries = download_retries + 1
        if download_retries <= MAX_RETRIES then
            print("[STORE] Retry " .. download_retries .. "/" .. MAX_RETRIES)
            download_received = 0

            if download_stage == "firmware" then
                start_firmware_update()
            elseif download_app then
                install_app(download_app)
            end
        else
            error_msg = "Failed after " .. MAX_RETRIES .. " attempts"
            ui.toast(error_msg, ui.TOAST_ERROR)
            current_screen = SCR_BROWSE
        end
    end

    -- Draw
    local needs_draw = (current_screen ~= last_screen)
    if current_screen == SCR_DOWNLOADING or current_screen == SCR_LOADING then
        frame = frame + 1
        needs_draw = true
    end

    if needs_draw then
        if current_screen == SCR_LOADING then draw_loading()
        elseif current_screen == SCR_NO_WIFI then draw_no_wifi()
        elseif current_screen == SCR_BROWSE then draw_browse()
        elseif current_screen == SCR_UPDATES then draw_updates()
        elseif current_screen == SCR_INSTALLED then draw_installed()
        elseif current_screen == SCR_DETAIL then draw_detail()
        elseif current_screen == SCR_DOWNLOADING then draw_downloading()
        elseif current_screen == SCR_FW_CONFIRM then draw_fw_confirm()
        end
        last_screen = current_screen
    end

    sys.sleep(33)  -- ~30 FPS
end
