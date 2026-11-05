-- SSH Client for PicoDeck
-- Full SSH-2 terminal client with VT100 emulation

local pc = picocalc
local disp = pc.display
local inp = pc.input
local fs = pc.fs
local sys = pc.sys
local ui = pc.ui
local net = pc.network
local tcp = pc.tcp

-- Module loader (package/require are blocked by the sandbox)
local _loaded = {}
function require(name)
    if _loaded[name] then return _loaded[name] end
    local path = APP_DIR .. "/" .. name:gsub("%.", "/") .. ".lua"
    local src = fs.readFile(path)
    if not src then error("require: file not found: " .. path) end
    local fn, err = load(src, "@" .. path)
    if not fn then error("require: " .. err) end
    local result = fn()
    if result == nil then result = true end
    _loaded[name] = result
    return result
end

-- Load SSH libraries
local wire = require("lib.ssh_wire")
local ssh_transport = require("lib.ssh_transport")
local ssh_auth = require("lib.ssh_auth")
local ssh_connection = require("lib.ssh_connection")
local ssh_input = require("lib.ssh_input")

-- ── Known hosts management ──────────────────────────────────────────────────

local DATA_DIR = "/data/net.picodeck.ssh"
local KNOWN_HOSTS_PATH = DATA_DIR .. "/known_hosts"
local CONNECTIONS_PATH = DATA_DIR .. "/connections.json"

local function ensure_data_dir()
    if not fs.exists(DATA_DIR) then
        fs.mkdir(DATA_DIR)
    end
end

local function load_known_hosts()
    local hosts = {}
    local content = fs.readFile(KNOWN_HOSTS_PATH)
    if not content then return hosts end
    for line in content:gmatch("[^\n]+") do
        local host, algo, fingerprint = line:match("^(%S+)%s+(%S+)%s+(%S+)")
        if host then
            hosts[host] = {algo = algo, fingerprint = fingerprint}
        end
    end
    return hosts
end

local function save_known_host(host_key, algo, fingerprint)
    ensure_data_dir()
    local f = fs.open(KNOWN_HOSTS_PATH, "a")
    if f then
        fs.write(f, host_key .. " " .. algo .. " " .. fingerprint .. "\n")
        fs.close(f)
    end
end

local function json_escape(s)
    return s:gsub('\\', '\\\\'):gsub('"', '\\"')
end

local function load_connections()
    local content = fs.readFile(CONNECTIONS_PATH)
    if not content then return {} end
    local conns = {}
    -- Match each {...} object in the array
    for obj in content:gmatch('{([^}]+)}') do
        local c = {}
        c.host = obj:match('"host"%s*:%s*"([^"]*)"')
        local p = obj:match('"port"%s*:%s*(%d+)')
        c.port = tonumber(p) or 22
        c.user = obj:match('"user"%s*:%s*"([^"]*)"')
        c.name = obj:match('"name"%s*:%s*"([^"]*)"')
        c.password = obj:match('"password"%s*:%s*"([^"]*)"')
        if c.host and c.user then
            conns[#conns + 1] = c
        end
    end
    return conns
end

local function save_connections(conns)
    ensure_data_dir()
    local parts = {}
    for _, c in ipairs(conns) do
        local fields = {}
        if c.name and #c.name > 0 then
            fields[#fields + 1] = string.format('"name":"%s"', json_escape(c.name))
        end
        fields[#fields + 1] = string.format('"host":"%s"', json_escape(c.host))
        fields[#fields + 1] = string.format('"port":%d', c.port)
        fields[#fields + 1] = string.format('"user":"%s"', json_escape(c.user))
        if c.password and #c.password > 0 then
            fields[#fields + 1] = string.format('"password":"%s"', json_escape(c.password))
        end
        parts[#parts + 1] = "{" .. table.concat(fields, ",") .. "}"
    end
    local json = "[" .. table.concat(parts, ",") .. "]"
    local f = fs.open(CONNECTIONS_PATH, "w")
    if f then
        fs.write(f, json)
        fs.close(f)
    end
end

local function connection_label(c)
    if c.name and #c.name > 0 then
        return c.name .. " (" .. c.user .. "@" .. c.host .. ":" .. c.port .. ")"
    end
    return c.user .. "@" .. c.host .. ":" .. c.port
end

local function edit_profile(conn)
    -- Returns updated connection table, or nil if cancelled
    local name = ui.textInput("Name (optional)", conn and conn.name or "")
    -- name can be empty, that's fine

    local host = ui.textInput("Host", conn and conn.host or "")
    if not host or #host == 0 then return nil end

    local port_str = ui.textInput("Port", conn and tostring(conn.port) or "22")
    local port = tonumber(port_str) or 22

    local username = ui.textInput("Username", conn and conn.user or "")
    if not username or #username == 0 then return nil end

    local password = nil
    if ui.confirm("Save password in profile?") then
        password = ui.textInput("Password", "")
        if not password then return nil end
    end

    return {
        name = (name and #name > 0) and name or nil,
        host = host,
        port = port,
        user = username,
        password = (password and #password > 0) and password or nil,
    }
end

-- ── Status display ──────────────────────────────────────────────────────────

local function show_status(msg)
    disp.clear(0x0000)
    disp.drawText(10, 10, "SSH Client", 0xFFFF, 0x0000)
    disp.drawText(10, 30, msg, 0x07E0, 0x0000)  -- green
    disp.flush()
end

local function show_error(msg)
    disp.clear(0x0000)
    disp.drawText(10, 10, "SSH Error", 0xF800, 0x0000)  -- red

    -- Word wrap
    local y = 30
    local line = ""
    for word in msg:gmatch("%S+") do
        if #line + #word + 1 > 50 then
            disp.drawText(10, y, line, 0xFFFF, 0x0000)
            y = y + 10
            line = word
        else
            line = (#line > 0) and (line .. " " .. word) or word
        end
    end
    if #line > 0 then
        disp.drawText(10, y, line, 0xFFFF, 0x0000)
    end

    disp.drawText(10, 300, "Press any key to continue", 0x7BEF, 0x0000)
    disp.flush()

    -- Wait for keypress
    sys.sleep(200)
    while true do
        inp.update()
        if inp.getButtonsPressed() ~= 0 or inp.getChar() then
            break
        end
        sys.sleep(16)
    end
end

-- ── Connection UI ───────────────────────────────────────────────────────────

local function draw_connection_list(conns, selected)
    disp.clear(0x0000)
    disp.drawText(10, 10, "SSH Connections", 0xFFFF, 0x0000)
    disp.drawText(10, 26, string.rep("-", 50), 0x7BEF, 0x0000)

    if #conns == 0 then
        disp.drawText(10, 44, "No saved connections.", 0x7BEF, 0x0000)
    else
        for i, c in ipairs(conns) do
            local label = string.format("%d. %s", i, connection_label(c))
            if #label > 50 then label = label:sub(1, 47) .. "..." end
            local color = (i == selected) and 0x07E0 or 0xFFE0
            local prefix = (i == selected) and "> " or "  "
            disp.drawText(4, 34 + i * 12, prefix .. label, color, 0x0000)
        end
    end

    disp.drawText(10, 290, "N:New  E:Edit  D:Delete", 0x7BEF, 0x0000)
    disp.drawText(10, 304, "1-9/Enter:Connect  Esc:Quit", 0x7BEF, 0x0000)
    disp.flush()
end

local function get_connection_params()
    local conns = load_connections()
    local selected = (#conns > 0) and 1 or 0

    -- If no saved connections, go straight to new profile
    if #conns == 0 then
        local profile = edit_profile(nil)
        if not profile then return nil end
        conns[1] = profile
        save_connections(conns)
        -- Connect with this new profile
        local pw = profile.password
        if not pw then
            pw = ui.textInput("Password", "")
            if not pw then return nil end
        end
        return profile.host, profile.port, profile.user, pw, 1
    end

    draw_connection_list(conns, selected)

    while true do
        inp.update()
        local ch = inp.getChar()
        local btns = inp.getButtonsPressed()

        if ch then
            local b = string.byte(ch)
            -- Number keys 1-9: select and connect
            if b >= string.byte("1") and b <= string.byte("9") then
                local idx = b - string.byte("0")
                if idx <= #conns then
                    selected = idx
                    local c = conns[selected]
                    local pw = c.password
                    if not pw then
                        pw = ui.textInput("Password", "")
                        if not pw then
                            draw_connection_list(conns, selected)
                            goto continue
                        end
                    end
                    return c.host, c.port, c.user, pw, selected
                end

            elseif ch == "n" or ch == "N" then
                local profile = edit_profile(nil)
                if profile then
                    conns[#conns + 1] = profile
                    save_connections(conns)
                    selected = #conns
                end
                draw_connection_list(conns, selected)

            elseif ch == "e" or ch == "E" then
                if selected > 0 and selected <= #conns then
                    local profile = edit_profile(conns[selected])
                    if profile then
                        conns[selected] = profile
                        save_connections(conns)
                    end
                    draw_connection_list(conns, selected)
                end

            elseif ch == "d" or ch == "D" then
                if selected > 0 and selected <= #conns then
                    local label = connection_label(conns[selected])
                    if ui.confirm("Delete " .. label .. "?") then
                        table.remove(conns, selected)
                        save_connections(conns)
                        if selected > #conns then selected = #conns end
                        if selected == 0 then
                            -- No connections left, prompt for new
                            local profile = edit_profile(nil)
                            if not profile then return nil end
                            conns[1] = profile
                            save_connections(conns)
                            selected = 1
                        end
                    end
                    draw_connection_list(conns, selected)
                end
            end
        end

        -- Enter: connect with selected
        if btns & inp.BTN_ENTER ~= 0 then
            if selected > 0 and selected <= #conns then
                local c = conns[selected]
                local pw = c.password
                if not pw then
                    pw = ui.textInput("Password", "")
                    if not pw then
                        draw_connection_list(conns, selected)
                        goto continue
                    end
                end
                return c.host, c.port, c.user, pw, selected
            end
        end

        -- Arrow keys to move selection
        if btns & inp.BTN_UP ~= 0 then
            if selected > 1 then
                selected = selected - 1
                draw_connection_list(conns, selected)
            end
        elseif btns & inp.BTN_DOWN ~= 0 then
            if selected < #conns then
                selected = selected + 1
                draw_connection_list(conns, selected)
            end
        end

        if btns & inp.BTN_ESC ~= 0 then
            return nil
        end

        ::continue::
        sys.sleep(16)
    end
end

-- ── Host key verification ───────────────────────────────────────────────────

local function verify_host_key(transport, host, port)
    local fingerprint = transport:get_host_key_fingerprint()
    local algo = transport:get_host_key_type()
    if not fingerprint or not algo then
        show_error("Could not get host key fingerprint")
        return false
    end

    local host_key = host .. ":" .. port
    local known = load_known_hosts()

    if known[host_key] then
        if known[host_key].fingerprint == fingerprint then
            return true  -- known and matches
        else
            -- KEY CHANGED!
            disp.clear(0x0000)
            disp.drawText(10, 10, "WARNING: HOST KEY CHANGED!", 0xF800, 0x0000)
            disp.drawText(10, 30, "This could be a MITM attack.", 0xFFFF, 0x0000)
            disp.drawText(10, 50, "Old: " .. known[host_key].fingerprint:sub(1, 30), 0xFBE0, 0x0000)
            disp.drawText(10, 62, "New: " .. fingerprint:sub(1, 30), 0xFBE0, 0x0000)
            disp.drawText(10, 300, "Press Y to accept, any other to abort", 0x7BEF, 0x0000)
            disp.flush()

            while true do
                inp.update()
                local ch = inp.getChar()
                if ch == "y" or ch == "Y" then
                    -- Update known host
                    -- Rewrite file (simple: just re-save)
                    known[host_key] = {algo = algo, fingerprint = fingerprint}
                    ensure_data_dir()
                    local f = fs.open(KNOWN_HOSTS_PATH, "w")
                    if f then
                        for k, v in pairs(known) do
                            fs.write(f, k .. " " .. v.algo .. " " .. v.fingerprint .. "\n")
                        end
                        fs.close(f)
                    end
                    return true
                elseif ch or inp.getButtonsPressed() ~= 0 then
                    return false
                end
                sys.sleep(16)
            end
        end
    else
        -- New host — show fingerprint and ask
        disp.clear(0x0000)
        disp.drawText(10, 10, "New SSH Host", 0xFFFF, 0x0000)
        disp.drawText(10, 30, host .. ":" .. port, 0xFFE0, 0x0000)
        disp.drawText(10, 50, "Key type: " .. algo, 0x7BEF, 0x0000)
        disp.drawText(10, 70, "Fingerprint:", 0x7BEF, 0x0000)

        -- Display fingerprint in two lines (it's long)
        local fp = fingerprint
        if #fp > 48 then
            disp.drawText(10, 85, fp:sub(1, 48), 0x07E0, 0x0000)
            disp.drawText(10, 97, fp:sub(49), 0x07E0, 0x0000)
        else
            disp.drawText(10, 85, fp, 0x07E0, 0x0000)
        end

        disp.drawText(10, 300, "Press Y to accept, any other to abort", 0x7BEF, 0x0000)
        disp.flush()

        while true do
            inp.update()
            local ch = inp.getChar()
            if ch == "y" or ch == "Y" then
                save_known_host(host_key, algo, fingerprint)
                return true
            elseif ch or inp.getButtonsPressed() ~= 0 then
                return false
            end
            sys.sleep(16)
        end
    end
end

-- ── Terminal session ────────────────────────────────────────────────────────

local function run_terminal_session(channel, transport)
    local term = pc.terminal.new(53, 26, 500)
    term:setCursorVisible(true)
    term:setCursorBlink(true)
    term:render()
    disp.flush()

    local decckm = false  -- cursor key mode (set by DEC private mode ?1h/l)

    while not channel.closed do
        -- Poll keyboard
        inp.update()

        -- Check for Esc held > 500ms to disconnect (quick Esc sends ESC char)
        local buttons_pressed = inp.getButtonsPressed()

        -- Translate character input
        local ch = inp.getChar()
        if ch then
            local translated = ssh_input.translate_char(ch)
            if translated then
                channel:write(translated)
            end
        end

        -- Translate button presses (arrows, function keys, etc.)
        -- But skip if a char was already sent (avoid double-sending Enter, Backspace, etc.)
        if not ch and buttons_pressed ~= 0 then
            local seq = ssh_input.translate_buttons(buttons_pressed, decckm)
            if #seq > 0 then
                channel:write(seq)
            end
        end

        -- Poll for incoming SSH data
        channel:poll(0)

        -- Feed data to terminal
        local data = channel:read()
        if data then
            decckm = ssh_input.track_cursor_keys(data, decckm)
            term:write(data)
        end

        -- Render dirty regions
        term:renderDirty()
        disp.flush()

        -- Small sleep to prevent busy-waiting
        sys.sleep(5)
    end

    term:setCursorVisible(false)
end

-- ── Main ────────────────────────────────────────────────────────────────────

local function main()
    -- Check WiFi
    if net.getStatus() ~= net.kStatusConnected then
        show_error("WiFi not connected. Please connect WiFi first.")
        return
    end

    -- Get connection parameters
    local host, port, user, password, conn_idx = get_connection_params()
    if not host then return end

    -- Connect TCP
    show_status("Connecting to " .. host .. ":" .. port .. "...")

    local conn = tcp.new(host, port, false)
    if not conn then
        show_error("Failed to create TCP connection")
        return
    end

    local ok, err = conn:connect()
    if not ok then
        show_error("TCP connect failed: " .. (err or "unknown"))
        conn:close()
        return
    end

    -- Wait for TCP connection
    if not conn:waitConnected(15) then
        show_error("Connection timed out")
        conn:close()
        return
    end

    show_status("Connected. Starting SSH handshake...")

    -- SSH transport handshake
    local t = ssh_transport.new(conn)
    if not t:handshake() then
        show_error("SSH handshake failed: " .. (t.error or "unknown"))
        t:close()
        conn:close()
        return
    end

    show_status("Handshake OK. Verifying host key...")

    -- Host key verification
    if not verify_host_key(t, host, port) then
        show_error("Host key rejected")
        t:close()
        conn:close()
        return
    end

    show_status("Authenticating...")

    -- Authentication
    if not ssh_auth.request_service(t) then
        show_error("Service request failed: " .. (t.error or "unknown"))
        t:close()
        conn:close()
        return
    end

    local auth_ok, banner = ssh_auth.password(t, user, password)

    if not auth_ok then
        password = nil
        show_error("Authentication failed: " .. (t.error or "wrong password"))
        t:close()
        conn:close()
        return
    end

    -- Offer to save password if it wasn't already saved in the profile
    local conns = load_connections()
    if conn_idx and conn_idx <= #conns then
        if not conns[conn_idx].password and password and #password > 0 then
            if ui.confirm("Save password to profile?") then
                conns[conn_idx].password = password
                save_connections(conns)
            end
        end
    end
    password = nil  -- clear from memory

    show_status("Opening session...")

    -- Open channel
    local channel = ssh_connection.new(t)
    if not channel:open_session() then
        show_error("Channel open failed: " .. (channel.error or "unknown"))
        t:close()
        conn:close()
        return
    end

    -- Request PTY
    if not channel:request_pty("xterm-256color", 53, 26) then
        show_error("PTY request failed: " .. (channel.error or "unknown"))
        channel:close()
        t:close()
        conn:close()
        return
    end

    -- Request shell
    if not channel:request_shell() then
        show_error("Shell request failed: " .. (channel.error or "unknown"))
        channel:close()
        t:close()
        conn:close()
        return
    end

    -- Run terminal session
    run_terminal_session(channel, t)

    -- Cleanup
    local session_error = channel.error
    channel:close()
    t:close()
    conn:close()

    if session_error then
        show_error("Disconnected: " .. session_error)
        return
    end
    show_status("Disconnected.")
    sys.sleep(1000)
end

-- Run the app
local ok, err = pcall(main)
if not ok then
    show_error("Fatal error: " .. tostring(err))
end
