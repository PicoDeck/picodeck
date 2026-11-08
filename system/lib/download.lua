-- PicoDeck download library — stream an HTTP(S) response straight to a file.
-- Load with: local download = picocalc.sys.loadlib("download")
--
--   local ok, err = download.toFile(url, dest_path, {
--       onProgress   = function(received, total) end, -- total 0 if unknown
--       headers      = { ["X-Extra"] = "..." },       -- merged into request
--       timeoutMs    = 120000,                        -- whole-transfer cap
--       maxRedirects = 5,
--   })
--
-- Blocking: returns only when the transfer finished, failed, or timed out.
-- Chunks are written to the SD card as they arrive under fs.setSlowMode(true)
-- (SPI0 drops to 1 MHz — CYW43 radio EMI corrupts 25 MHz writes), so memory use
-- does not grow with the file: one receive ring (256 KB, 32 KB if the heap
-- cannot spare that) and a 32 KB read at a time. The file is written as
-- dest .. ".part" and takes dest's place only once it is complete: on any
-- failure the .part is deleted and a file already at dest is left as it was.
--
-- The caller's app needs the "http" requirement and a connected network.
-- HTTP callbacks are fired from the Lua debug hook, so this library must be
-- called from plain Lua code (not from inside another network callback).

local pc   = picocalc
local fs   = pc.fs
local sys  = pc.sys
local net  = pc.network

local D = {}

-- One read's worth: what fs.write sends to the card at a time.
local READ_CHUNK = 32 * 1024

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

-- The URL a Location header points at, seen from the URL that sent it. A
-- Location may be a whole URL or a reference relative to it (RFC 9110).
local function resolve_location(base, location)
    if location:match("^%a[%w+.-]*:") then return location end
    local scheme, authority, path = base:match("^(%a+)://([^/?#]+)([^#]*)$")
    if not scheme then return location end
    if location:sub(1, 2) == "//" then return scheme .. ":" .. location end
    local origin = scheme .. "://" .. authority
    if location:sub(1, 1) == "/" then return origin .. location end
    local base_path = path:match("^[^?]*")
    if base_path == "" then base_path = "/" end
    if location:sub(1, 1) == "?" then return origin .. base_path .. location end
    -- Relative to the base's directory; "." and ".." segments collapse.
    local rel, tail = location:match("^([^?#]*)(.*)$")
    local merged = base_path:match("^(.*/)") .. rel
    local segments = {}
    for name in merged:gmatch("[^/]+") do
        if name == ".." then
            segments[#segments] = nil
        elseif name ~= "." then
            segments[#segments + 1] = name
        end
    end
    local joined = "/" .. table.concat(segments, "/")
    if #segments > 0 and (merged:sub(-1) == "/" or merged:match("/%.%.?$")) then
        joined = joined .. "/"
    end
    return origin .. joined .. tail
end

-- Ensure every directory component of dest exists.
local function ensure_parent_dirs(dest)
    local dir = dest:match("^(.+)/[^/]+$")
    if not dir or dir == "" then return end
    local built = ""
    for part in dir:gmatch("[^/]+") do
        built = built .. "/" .. part
        fs.mkdir(built)
    end
end

-- Put the finished .part where dest is. FatFS refuses a rename onto an existing
-- file, so an old dest steps aside first and comes back if the rename fails.
local function install(part, dest)
    local old = nil
    if fs.exists(dest) then
        old = dest .. ".old"
        fs.delete(old)
        if not fs.rename(dest, old) then
            fs.delete(part)
            return false, "cannot replace " .. dest
        end
    end
    if not fs.rename(part, dest) then
        if old then fs.rename(old, dest) end
        fs.delete(part)
        return false, "cannot rename " .. part
    end
    if old then fs.delete(old) end
    return true
end

-- One request/response leg. Returns:
--   "done"               transfer complete, file in place
--   "redirect", location follow the Location header
--   nil, err             failure (partial file already cleaned up)
local function fetch_leg(url, dest, opts, deadline)
    local host, port, ssl, path = parse_url(url)
    if not host then return nil, "invalid URL: " .. tostring(url) end

    local conn = net.http.new(host, port, ssl, "download.lua")
    if not conn then return nil, "cannot connect to " .. host end

    conn:setConnectTimeout(30)
    conn:setReadTimeout(60)
    -- A large ring (PSRAM) absorbs the SD write's pauses; past it the device
    -- slows the server to the write's pace (TCP flow control).  Fall back to
    -- 32 KB if the heap cannot spare it.
    if not conn:setReadBufferSize(256 * 1024) then
        conn:setReadBufferSize(32 * 1024)
    end

    local part = dest .. ".part"
    local state = {
        done = false, err = nil, redirect = nil,
        wf = nil, received = 0, total = 0, status = nil,
    }

    local function cleanup_partial()
        if state.wf then
            fs.close(state.wf)
            state.wf = nil
            fs.delete(part)
        end
        fs.setSlowMode(false)
    end

    local function fail(err)
        if state.done then return end
        state.err = err
        state.done = true
        cleanup_partial()
    end

    -- Move what has arrived to the file. False once the transfer has failed.
    local function drain()
        while conn:getBytesAvailable() > 0 do
            local data = conn:read(READ_CHUNK)
            if not data or #data == 0 then break end
            -- FatFS reports a full card as success with fewer bytes written.
            local n = fs.write(state.wf, data)
            if n ~= #data then
                fail("SD write failed (card full?)")
                conn:close()
                return false
            end
            state.received = state.received + n
            if opts.onProgress then
                pcall(opts.onProgress, state.received, state.total)
            end
            if sys.getTimeMs() > deadline then
                fail("timeout")
                conn:close()
                return false
            end
        end
        return true
    end

    conn:setHeadersReadCallback(function()
        local hdrs = conn:getResponseHeaders()
        if hdrs and hdrs["content-length"] then
            local cl = tonumber(hdrs["content-length"])
            if cl and cl > 0 then state.total = cl end
        end
    end)

    conn:setRequestCallback(function()
        if state.done then return end
        local status = conn:getResponseStatus()
        state.status = status
        if status and status ~= 200 then -- redirect/error: its body is not wanted
            while conn:read(READ_CHUNK) do end
            return
        end

        if not state.wf then
            ensure_parent_dirs(dest)
            if not fs.ensureReady() then
                fail("SD card not responding")
                conn:close()
                return
            end
            if fs.isDir(dest) then
                fail("cannot open " .. dest)
                conn:close()
                return
            end
            fs.setSlowMode(true) -- radio is active for the whole transfer
            state.wf = fs.open(part, "w")
            if not state.wf then
                fail("cannot open " .. dest)
                conn:close()
                return
            end
        end

        drain()
    end)

    conn:setRequestCompleteCallback(function()
        if state.done then return end
        local status = conn:getResponseStatus()
        local conn_err = conn:getError()

        if status and status >= 300 and status < 400 then
            local hdrs = conn:getResponseHeaders()
            local location = hdrs and hdrs["location"]
            cleanup_partial()
            if location then
                state.redirect = resolve_location(url, location)
            else
                state.err = "redirect without Location"
            end
            state.done = true
            return
        end

        if conn_err then return fail(conn_err) end
        if status and status ~= 200 then
            return fail("HTTP " .. tostring(status))
        end
        if not state.wf then
            return fail("empty response")
        end
        if not drain() then return end

        -- Everything the connection received is in the file, and the card
        -- holds all of it: closing flushes, and fs.close reports nothing.
        local got = conn:getProgress()
        fs.close(state.wf); state.wf = nil
        fs.setSlowMode(false)
        if state.received ~= got or fs.size(part) ~= state.received then
            fs.delete(part)
            return fail("SD write failed (card full?)")
        end
        local ok, err = install(part, dest)
        if not ok then return fail(err) end
        if opts.onProgress then
            pcall(opts.onProgress, state.received,
                  state.total > 0 and state.total or state.received)
        end
        state.done = true
    end)

    conn:setConnectionClosedCallback(function()
        if not state.done then fail(conn:getError() or "connection closed") end
    end)

    local headers = { ["User-Agent"] = "PicoDeck-download/1.0" }
    if opts.headers then
        for k, v in pairs(opts.headers) do headers[k] = v end
    end
    local started, why = conn:get(path, headers)
    if not started then fail(why or "request refused") end

    -- Block until a callback resolves the transfer. Callbacks fire from the
    -- Lua debug hook, which needs Lua opcodes executing — hence the loop.
    while not state.done do
        if sys.getTimeMs() > deadline then
            conn:close()
            fail("timeout")
            break
        end
        sys.sleep(5)
    end
    conn:close()

    if state.redirect then return "redirect", state.redirect end
    if state.err then return nil, state.err end
    return "done"
end

function D.toFile(url, dest, opts)
    opts = opts or {}
    local max_redirects = opts.maxRedirects or 5
    local deadline = sys.getTimeMs() + (opts.timeoutMs or 120000)
    fs.delete(dest .. ".part")  -- what an interrupted run left

    for _ = 0, max_redirects do
        local res, arg = fetch_leg(url, dest, opts, deadline)
        if res == "done" then return true end
        if res == "redirect" then
            url = arg
        else
            return false, arg
        end
    end
    return false, "too many redirects"
end

return D
