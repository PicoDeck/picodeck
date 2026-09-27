-- Wikipedia App for PicoDeck
-- Fetches from Wikipedia TextExtracts API

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local sys   = pc.sys
local net   = pc.network

-- ── Constants ────────────────────────────────────────────────────────────────
local SCREEN_W = disp.getWidth()
local SCREEN_H = disp.getHeight()
local CHAR_W = 6
local CHAR_H = 8
local LINE_SPACING = 12  -- 1.5x line spacing
local COLS = math.floor(SCREEN_W / CHAR_W)
local ROWS = math.floor(SCREEN_H / CHAR_H)

-- Colors
local BG        = disp.BLACK
local FG        = disp.WHITE
local TITLE_BG  = disp.rgb(60, 60, 60)
local TITLE_FG  = disp.YELLOW
local STATUS_BG = disp.rgb(0, 60, 120)
local STATUS_FG = disp.WHITE
local CURSOR_C  = disp.CYAN
local HIGHLIGHT = disp.rgb(80, 80, 0)

-- Screens
local SCR_HOME    = 1
local SCR_SEARCH  = 2
local SCR_ARTICLE = 3
local SCR_LOADING = 4
local SCR_ERROR   = 5
local SCR_WIFI_WAIT = 7

-- ── State ─────────────────────────────────────────────────────────────────────
local current_screen = SCR_WIFI_WAIT
local featured_articles = {}
local search_results = {}
local search_query = ""
local article_title = ""
local article_text = ""
local article_lines = {}
local scroll_y = 0
local loading_msg = ""
local error_msg = ""
local selected_idx = 1
local current_conn = nil
local wiki_conn = nil  -- Persistent HTTPS connection to en.wikipedia.org
local loading_start_time = 0

-- Loading animation state
local spin_chars = {"|", "/", "-", "\\"}
local spin_idx = 1
local last_spin_time = 0

-- ── Network Helpers ───────────────────────────────────────────────────────────

local function url_encode(str)
    if not str then return "" end
    str = str:gsub("\n", "\r\n")
    str = str:gsub("([^%w %-%_%.%~])", function(c)
        return string.format("%%%02X", string.byte(c))
    end)
    str = str:gsub(" ", "+")
    return str
end

-- Minimal JSON-ish extractor for specific keys
-- Extracts values for keys like "title", "extract", "search"
local function json_extract_list(json, key)
    local results = {}
    -- Find array patterns like "key":[{"title":"..."}, ...]
    local pattern = '"' .. key .. '":%s*%[([^%]]+)%]'
    local list_str = json:match(pattern)
    if list_str then
        for item in list_str:gmatch('{[^}]+}') do
            local val = item:match('"title":%s*"([^"]+)"')
            if val then table.insert(results, val) end
        end
    end
    return results
end

local function json_extract_val(json, key)
    local pattern = '"' .. key .. '":%s*"([^"]+)"'
    local val = json:match(pattern)
    if val then
        -- Unescape some common JSON escapes
        val = val:gsub("\\n", "\n")
        val = val:gsub("\\\"", '"')
        val = val:gsub("\\\\", "\\")
        val = val:gsub("\\u%x%x%x%x", "")  -- strip non-displayable unicode escapes
    end
    return val
end

-- Async HTTP fetch helper
local function fetch_json(url_path, callback)
    sys.log("HTTP: Fetching " .. url_path)

    if wiki_conn == nil then
        local conn, err = net.http.new("en.wikipedia.org", 443, true)
        if not conn then
            current_screen = SCR_ERROR
            error_msg = "Conn Failed: " .. (err or "unknown")
            return
        end
        conn:setKeepAlive(true)
        conn:setReadBufferSize(32768)
        wiki_conn = conn
    end

    local conn = wiki_conn
    current_conn = conn
    local body = ""
    conn:setRequestCallback(function()
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            body = body .. conn:read()
        end
    end)

    conn:setRequestCompleteCallback(function()
        local status = conn:getResponseStatus()
        sys.log("HTTP: Complete, status=" .. tostring(status))
        if status == 200 then
            callback(body)
        elseif status == 301 or status == 302 then
            current_screen = SCR_ERROR
            error_msg = "Redirected to HTTPS.\nEnable SSL in main.lua\nor use an HTTP mirror."
            sys.log("HTTP: Redirected")
        else
            current_screen = SCR_ERROR
            error_msg = "HTTP Error: " .. tostring(status)
            sys.log("HTTP: Error status " .. tostring(status))
        end
    end)

    conn:setConnectionClosedCallback(function()
        sys.log("HTTP: Connection closed")
        if current_conn == conn then current_conn = nil end
        wiki_conn = nil  -- allow reconnect on next request
        if current_screen == SCR_LOADING then
            local status = conn:getResponseStatus()
            if status == 301 or status == 302 then
                current_screen = SCR_ERROR
                error_msg = "Redirected to HTTPS.\nTry an HTTP mirror like\n'wp.aks.net.pl' on port 80."
            else
                current_screen = SCR_ERROR
                error_msg = "Connection closed before\ndata was received."
            end
        end
    end)

    local headers = {
        ["User-Agent"] = "PicoDeck-Wikipedia/1.0 (ClockworkPi PicoCalc)",
        ["Accept"] = "application/json",
    }

    if not conn:get(url_path, headers) then
        current_screen = SCR_ERROR
        error_msg = "GET Failed"
        sys.log("HTTP: GET initiation failed")
    end
end

-- ── Business Logic ────────────────────────────────────────────────────────────

local function wrap_text(text, width)
    local lines = {}
    for line in text:gmatch("([^\n]*)\n?") do
        if #line == 0 then
            table.insert(lines, "")
        else
            while #line > width do
                local split = width
                -- Try to split at space
                local space = line:sub(1, width):match(".*%s()")
                if space and space > 1 then
                    split = space - 1
                end
                table.insert(lines, line:sub(1, split))
                line = line:sub(split + 1):gsub("^%s*", "")
            end
            table.insert(lines, line)
        end
    end
    return lines
end

local function truncate(text, max_len)
    if #text <= max_len then return text end
    return text:sub(1, max_len - 3) .. "..."
end

local function load_article(title)
    current_screen = SCR_LOADING
    loading_start_time = sys.getTimeMs()
    loading_msg = "Loading Article..."
    article_title = title
    
    -- Removed exintro to get full content, added exchars for a reasonable large limit
    local path = "/w/api.php?action=query&prop=extracts&explaintext&exchars=20000&titles=" .. url_encode(title) .. "&format=json&origin=*"
    fetch_json(path, function(json)
        local extract = json_extract_val(json, "extract")
        if extract then
            article_text = extract
            article_lines = wrap_text(article_text, COLS - 2)
            scroll_y = 0
            current_screen = SCR_ARTICLE
        else
            current_screen = SCR_ERROR
            error_msg = "Article not found or empty."
        end
    end)
end

local function perform_search()
    if #search_query == 0 then return end
    current_screen = SCR_LOADING
    loading_start_time = sys.getTimeMs()
    loading_msg = "Searching..."
    
    local path = "/w/api.php?action=query&list=search&srsearch=" .. url_encode(search_query) .. "&format=json&origin=*"
    fetch_json(path, function(json)
        search_results = json_extract_list(json, "search")
        if #search_results > 0 then
            selected_idx = 1
            current_screen = SCR_SEARCH
        else
            current_screen = SCR_ERROR
            error_msg = "No results found."
        end
    end)
end

local function load_featured()
    current_screen = SCR_LOADING
    loading_start_time = sys.getTimeMs()
    loading_msg = "Loading Featured..."
    
    -- Wikipedia REST API for featured is usually HTTPS only.
    -- We'll use Random articles as "Featured" for PicoDeck (HTTP compatibility)
    local path = "/w/api.php?action=query&list=random&rnnamespace=0&rnlimit=15&format=json&origin=*"
    fetch_json(path, function(json)
        featured_articles = json_extract_list(json, "random")
        if #featured_articles > 0 then
            selected_idx = 1
            current_screen = SCR_HOME
        else
            current_screen = SCR_ERROR
            error_msg = "Failed to load articles."
        end
    end)
end

-- ── Drawing ───────────────────────────────────────────────────────────────────

-- Returns the first row below the header, plus a 1px gap. drawHeader
-- returns the rows it covers (older firmware returns nothing: 29 rows).
local function draw_header(title)
    return (pc.ui.drawHeader(title) or 29) + 1
end

local function draw_home()
    local content_y = draw_header("WIKIPEDIA")
    disp.drawText(2, content_y, "Featured Articles:", TITLE_FG, BG)
    
    for i, title in ipairs(featured_articles) do
        local y = content_y + CHAR_H * (1 + i)
        local display_title = truncate(title, COLS - 3)
        if i == selected_idx then
            disp.fillRect(0, y, SCREEN_W, CHAR_H, HIGHLIGHT)
            disp.drawText(2, y, "> " .. display_title, FG, HIGHLIGHT)
        else
            disp.drawText(2, y, "  " .. display_title, FG, BG)
        end
    end
    
    pc.ui.drawFooter("[S] Search  [R] Refresh", nil)
end

local function draw_search_results()
    local content_y = draw_header("Search: " .. search_query)
    
    for i, title in ipairs(search_results) do
        local y = content_y + CHAR_H * i
        local display_title = truncate(title, COLS - 3)
        if i == selected_idx then
            disp.fillRect(0, y, SCREEN_W, CHAR_H, HIGHLIGHT)
            disp.drawText(2, y, "> " .. display_title, FG, HIGHLIGHT)
        else
            disp.drawText(2, y, "  " .. display_title, FG, BG)
        end
    end
    
    pc.ui.drawFooter("[Esc] Back", nil)
end

local function draw_search_input()
    local content_y = draw_header("SEARCH")
    disp.drawText(2, content_y + CHAR_H * 2, "Enter query:", TITLE_FG, BG)
    disp.drawText(2, content_y + CHAR_H * 4, search_query .. "_", FG, BG)
    pc.ui.drawFooter("Enter: Search  Esc: Cancel", nil)
end

local function draw_article()
    local text_y = draw_header(article_title)
    local visible_rows = math.floor((SCREEN_H - CHAR_H * 3) / LINE_SPACING)
    
    for i = 1, visible_rows do
        local line_idx = scroll_y + i
        if line_idx <= #article_lines then
            disp.drawText(4, text_y + (i-1) * LINE_SPACING, article_lines[line_idx], FG, BG)
        end
    end
    
    -- Scrollbar
    local total_content_h = #article_lines * LINE_SPACING
    local visible_h = visible_rows * LINE_SPACING
    if total_content_h > visible_h then
        local sb_h = visible_h
        local thumb_h = math.max(4, math.floor(sb_h * (visible_h / total_content_h)))
        local thumb_y = text_y + math.floor((sb_h - thumb_h) * (scroll_y / (#article_lines - visible_rows)))
        disp.fillRect(SCREEN_W - 4, text_y, 2, sb_h, TITLE_BG)
        disp.fillRect(SCREEN_W - 4, thumb_y, 2, thumb_h, TITLE_FG)
    end
    
    -- Scroll percentage
    local scroll_pct = 0
    if #article_lines > visible_rows then
        scroll_pct = math.floor(100 * scroll_y / (#article_lines - visible_rows))
    end
    pc.ui.drawFooter("[Esc] Back  [←/→] PgUp/Dn  Scroll: " .. scroll_pct .. "%", nil)
end

local function draw_loading()
    local now = sys.getTimeMs()
    local elapsed_s = (now - loading_start_time) / 1000.0
    
    -- Update spin animation every 150ms
    if now - last_spin_time > 150 then
        spin_idx = (spin_idx % #spin_chars) + 1
        last_spin_time = now
    end
    
    local spin = spin_chars[spin_idx]
    local elapsed_str = string.format("%.1fs", elapsed_s)
    
    local msg = loading_msg .. " " .. spin
    local x = math.floor((SCREEN_W - #msg * CHAR_W) / 2)
    local y = SCREEN_H // 2 - 8
    disp.drawText(x, y, msg, TITLE_FG, BG)
    
    -- Show elapsed time
    local time_str = "Elapsed: " .. elapsed_str
    local time_x = math.floor((SCREEN_W - #time_str * CHAR_W) / 2)
    disp.drawText(time_x, y + 12, time_str, FG, BG)
    
    -- Show connection status
    local status_str = "Connecting to en.wikipedia.org..."
    local status_x = math.floor((SCREEN_W - #status_str * CHAR_W) / 2)
    disp.drawText(status_x, y + 28, status_str, BG, BG)
end

local function draw_error()
    draw_header("ERROR")
    disp.drawText(2, SCREEN_H // 2, error_msg, disp.RED, BG)
    pc.ui.drawFooter("Press any key to return", nil)
end

local function draw_wifi_wait()
    draw_header("WIKIPEDIA")
    disp.drawText(math.floor((SCREEN_W - 16 * CHAR_W) / 2), SCREEN_H // 2, "Waiting for WiFi...", TITLE_FG, BG)
end

-- ── Main Loop ─────────────────────────────────────────────────────────────────

local function main()
    local wifi_initialised = false
    
    while true do
        input.update()
        local pressed = input.getButtonsPressed()
        local char = input.getChar()
        
        if current_screen == SCR_WIFI_WAIT then
            if net.getStatus() == net.kStatusConnected then
                if not wifi_initialised then
                    wifi_initialised = true
                    load_featured()
                end
            elseif net.getStatus() == net.kStatusNotAvailable then
                current_screen = SCR_ERROR
                error_msg = "WiFi Hardware Not Available"
            end
            if pressed & input.BTN_ESC ~= 0 then
                sys.exit()
            end

        elseif current_screen == SCR_HOME then
            if pressed & input.BTN_UP ~= 0 then
                selected_idx = math.max(1, selected_idx - 1)
            elseif pressed & input.BTN_DOWN ~= 0 then
                selected_idx = math.min(#featured_articles, selected_idx + 1)
            elseif pressed & input.BTN_ENTER ~= 0 then
                load_article(featured_articles[selected_idx])
            elseif char == 's' or char == 'S' then
                search_query = ""
                current_screen = SCR_SEARCH_INPUT
            elseif char == 'r' or char == 'R' then
                load_featured()
            end
            
        elseif current_screen == SCR_SEARCH_INPUT then
            if pressed & input.BTN_ENTER ~= 0 then
                perform_search()
            elseif pressed & input.BTN_ESC ~= 0 then
                current_screen = SCR_HOME
            elseif pressed & input.BTN_BACKSPACE ~= 0 then
                search_query = search_query:sub(1, -2)
            elseif char and char ~= "" and #search_query < 30 then
                local b = string.byte(char)
                if b >= 32 and b <= 126 then
                    search_query = search_query .. char
                end
            end
            
        elseif current_screen == SCR_SEARCH then
            if pressed & input.BTN_UP ~= 0 then
                selected_idx = math.max(1, selected_idx - 1)
            elseif pressed & input.BTN_DOWN ~= 0 then
                selected_idx = math.min(#search_results, selected_idx + 1)
            elseif pressed & input.BTN_ENTER ~= 0 then
                load_article(search_results[selected_idx])
            elseif pressed & input.BTN_ESC ~= 0 then
                current_screen = SCR_HOME
            end
            
        elseif current_screen == SCR_ARTICLE then
            local visible_rows = math.floor((SCREEN_H - CHAR_H * 3) / LINE_SPACING)
            if pressed & input.BTN_UP ~= 0 then
                scroll_y = math.max(0, scroll_y - 1)
            elseif pressed & input.BTN_DOWN ~= 0 then
                scroll_y = math.min(math.max(0, #article_lines - visible_rows), scroll_y + 1)
            elseif pressed & input.BTN_LEFT ~= 0 then
                -- Page up
                scroll_y = math.max(0, scroll_y - visible_rows)
            elseif pressed & input.BTN_RIGHT ~= 0 then
                -- Page down
                scroll_y = math.min(math.max(0, #article_lines - visible_rows), scroll_y + visible_rows)
            elseif pressed & input.BTN_ESC ~= 0 then
                if #search_results > 0 then
                    current_screen = SCR_SEARCH
                else
                    current_screen = SCR_HOME
                end
            end
            
        elseif current_screen == SCR_LOADING then
            if pressed & input.BTN_ESC ~= 0 then
                if current_conn then
                    current_conn:close()
                    current_conn = nil
                end
                current_screen = SCR_HOME
            elseif sys.getTimeMs() - loading_start_time > 15000 then
                if current_conn then
                    current_conn:close()
                    current_conn = nil
                end
                current_screen = SCR_ERROR
                error_msg = "Request timed out."
            end
            
        elseif current_screen == SCR_ERROR then
            if pressed ~= 0 or char ~= "" then
                current_screen = SCR_HOME
            end
        end
        
        -- Drawing
        disp.clear(BG)
        if current_screen == SCR_HOME then draw_home()
        elseif current_screen == SCR_SEARCH_INPUT then draw_search_input()
        elseif current_screen == SCR_SEARCH then draw_search_results()
        elseif current_screen == SCR_ARTICLE then draw_article()
        elseif current_screen == SCR_LOADING then draw_loading()
        elseif current_screen == SCR_ERROR then draw_error()
        elseif current_screen == SCR_WIFI_WAIT then draw_wifi_wait()
        end
        disp.flush()
        
        sys.sleep(16)
    end
end

-- Need to add SCR_SEARCH_INPUT to constants
SCR_SEARCH_INPUT = 6

main()
