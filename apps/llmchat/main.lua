-- LLM Chat App for PicoDeck
-- Chat with OpenAI, Anthropic Claude, or Ollama

local pc    = picocalc
local disp  = pc.display
local input = pc.input
local sys   = pc.sys
local net   = pc.network
local cfg   = pc.config

-- ── Constants ────────────────────────────────────────────────────────────────
local SCREEN_W = disp.getWidth()
local SCREEN_H = disp.getHeight()
local CHAR_W = 6
local CHAR_H = 8

local COLS = math.floor(SCREEN_W / CHAR_W)
local ROWS = math.floor(SCREEN_H / CHAR_H)

local BG        = disp.BLACK
local FG        = disp.WHITE
local TITLE_BG  = disp.rgb(40, 40, 50)
local TITLE_FG  = disp.CYAN
local USER_BG   = disp.rgb(0, 60, 100)
local ASSIST_BG = disp.rgb(30, 30, 40)
local HIGHLIGHT = disp.rgb(80, 80, 0)
local ERROR_COL = disp.RED

-- Provider constants
local PROVIDER_OPENAI = 1
local PROVIDER_ANTHROPIC = 2
local PROVIDER_OLLAMA = 3

-- Screens
local SCR_WIFI_WAIT = 1
local SCR_MENU      = 2
local SCR_CHAT      = 3
local SCR_SETTINGS  = 4

-- ── State ─────────────────────────────────────────────────────────────────────
local current_screen = SCR_WIFI_WAIT
local wifi_initialised = false

-- Settings
local provider = PROVIDER_OPENAI
local api_key = ""
local api_endpoint = "https://api.openai.com/v1/chat/completions"
local model = "gpt-4o-mini"
local system_prompt = "You are a helpful assistant."

-- Chat state
local messages = {}  -- {role="user"|"assistant", content="..."}
local scroll_y = 0
local is_streaming = false
local current_conn = nil
local accumulated_response = ""

-- Menu state
local menu_items = {"Chat", "Settings", "Clear History", "Exit"}
local menu_selection = 1

-- Settings menu
local setting_items = {"Provider", "API Endpoint", "API Key", "Model", "System Prompt", "Back"}
local setting_selection = 1

-- Provider names
local provider_names = {"OpenAI Compatible", "Anthropic Claude", "Ollama (Local)"}

-- ── Config Helpers ──────────────────────────────────────────────────────────

local function load_config()
    provider = tonumber(cfg.get("llmchat_provider")) or PROVIDER_OPENAI
    api_key = cfg.get("llmchat_api_key") or ""
    api_endpoint = cfg.get("llmchat_endpoint") or "https://api.openai.com/v1/chat/completions"
    model = cfg.get("llmchat_model") or "gpt-4o-mini"
    system_prompt = cfg.get("llmchat_system") or "You are a helpful assistant."
    
    if provider == PROVIDER_ANTHROPIC then
        model = cfg.get("llmchat_model") or "claude-3-haiku-20240307"
    elseif provider == PROVIDER_OLLAMA then
        api_endpoint = cfg.get("llmchat_endpoint") or "http://192.168.1.1:11434/api/chat"
        model = cfg.get("llmchat_model") or "llama3"
    end
end

local function save_config()
    cfg.set("llmchat_provider", tostring(provider))
    cfg.set("llmchat_api_key", api_key)
    cfg.set("llmchat_endpoint", api_endpoint)
    cfg.set("llmchat_model", model)
    cfg.set("llmchat_system", system_prompt)
    cfg.save()
end

-- ── Network Helpers ──────────────────────────────────────────────────────────

-- Extract a JSON string value by key, properly handling escape sequences.
-- Simple [^"]* patterns break on content containing \"
local function extract_json_string(json, key)
    local pattern = '"' .. key .. '"%s*:%s*"'
    local _, pos = json:find(pattern)
    if not pos then return nil end
    pos = pos + 1
    local parts = {}
    while pos <= #json do
        local ch = json:sub(pos, pos)
        if ch == '\\' and pos < #json then
            local esc = json:sub(pos + 1, pos + 1)
            if esc == '"' then table.insert(parts, '"')
            elseif esc == 'n' then table.insert(parts, '\n')
            elseif esc == 'r' then table.insert(parts, '\r')
            elseif esc == 't' then table.insert(parts, '\t')
            elseif esc == '\\' then table.insert(parts, '\\')
            else table.insert(parts, esc)
            end
            pos = pos + 2
        elseif ch == '"' then
            return table.concat(parts)
        else
            table.insert(parts, ch)
            pos = pos + 1
        end
    end
    return nil
end

local function json_escape(s)
    if not s then return "" end
    s = s:gsub("\\", "\\\\")
    s = s:gsub('"', '\\"')
    s = s:gsub("\n", "\\n")
    s = s:gsub("\r", "\\r")
    s = s:gsub("\t", "\\t")
    return s
end

local function build_messages_json()
    local parts = {}
    -- Include system prompt as the first message
    if system_prompt ~= "" then
        table.insert(parts, string.format('{"role":"system","content":"%s"}', json_escape(system_prompt)))
    end
    for _, msg in ipairs(messages) do
        table.insert(parts, string.format('{"role":"%s","content":"%s"}', msg.role, json_escape(msg.content)))
    end
    return "[" .. table.concat(parts, ",") .. "]"
end

local function build_request_body()
    local msgs_json = build_messages_json()
    return string.format('{"model":"%s","messages":%s,"stream":false}', json_escape(model), msgs_json)
end

local function send_request(callback)
    is_streaming = true
    accumulated_response = ""
    
    local host, port, use_ssl, path
    
    if provider == PROVIDER_ANTHROPIC then
        host = "api.anthropic.com"
        port = 443
        use_ssl = true
        path = "/v1/messages"
    elseif provider == PROVIDER_OLLAMA then
        local ep = api_endpoint:gsub("https?://", ""):gsub("/.*", "")
        local pp = api_endpoint:gsub("https?://" .. ep, "")
        host = ep:gsub(":.*", "")
        port = tonumber(ep:match(":(%d+)$")) or 80
        use_ssl = api_endpoint:match("^https")
        path = pp == "" and "/api/chat" or pp
    else
        local ep = api_endpoint:gsub("https?://", ""):gsub("/.*", "")
        local pp = api_endpoint:gsub("https?://" .. ep, "")
        host = ep:gsub(":.*", "")
        port = tonumber(ep:match(":(%d+)$")) or (api_endpoint:match("^https") and 443 or 80)
        use_ssl = api_endpoint:match("^https")
        path = pp == "" and "/v1/chat/completions" or pp
    end
    
    local conn = net.http.new(host, port, use_ssl)
    if not conn then
        is_streaming = false
        return false
    end
    
    conn:setConnectTimeout(30)
    conn:setReadTimeout(60)
    conn:setReadBufferSize(65536)
    
    current_conn = conn
    
    local headers = {}
    
    if provider == PROVIDER_ANTHROPIC then
        headers["x-api-key"] = api_key
        headers["anthropic-version"] = "2023-06-01"
        headers["content-type"] = "application/json"
    else
        headers["Authorization"] = "Bearer " .. api_key
        headers["Content-Type"] = "application/json"
    end
    
    local body = build_request_body()
    sys.log("LLM: Sending request to " .. host .. path)
    
    local response_body = ""
    
    conn:setRequestCallback(function()
        local avail = conn:getBytesAvailable()
        if avail > 0 then
            local chunk = conn:read()
            if chunk then
                response_body = response_body .. chunk
            end
        end
    end)

    conn:setRequestCompleteCallback(function()
        local status = conn:getResponseStatus()
        sys.log("LLM: Response complete, status=" .. tostring(status) .. " body=" .. #response_body .. "B")

        if status == 200 then
            local content = nil

            if provider == PROVIDER_ANTHROPIC then
                content = extract_json_string(response_body, "text")
            else
                content = extract_json_string(response_body, "content")
            end

            if content and content ~= "" then
                callback(content)
            else
                sys.log("LLM: Parse failed, body preview: " .. response_body:sub(1, 200))
                callback("(Empty response)")
            end
        else
            local err = "HTTP " .. tostring(status)
            local msg = extract_json_string(response_body, "message")
            if msg then err = err .. ": " .. msg end
            callback(nil, err)
        end

        is_streaming = false
        current_conn = nil
    end)
    
    conn:setConnectionClosedCallback(function()
        sys.log("LLM: Connection closed")
        if not is_streaming then return end
        if accumulated_response ~= "" then
            callback(accumulated_response)
        else
            callback(nil, "Connection closed")
        end
        is_streaming = false
        current_conn = nil
    end)
    
    if not conn:post(path, headers, body) then
        is_streaming = false
        return false
    end
    
    return true
end

-- ── Drawing ───────────────────────────────────────────────────────────────────

-- Returns the rows the header covers (older firmware's drawHeader returns
-- nothing: 29 rows).
local function draw_header(title)
    return pc.ui.drawHeader(title) or 29
end

local function draw_wifi_wait()
    draw_header("LLM CHAT")
    disp.drawText(math.floor((SCREEN_W - 16 * CHAR_W) / 2), SCREEN_H // 2, "Waiting for WiFi...", TITLE_FG, BG)
end

local function draw_menu()
    local content_y = draw_header("LLM CHAT") + 6
    
    for i, item in ipairs(menu_items) do
        local y = content_y + i * 14
        if i == menu_selection then
            disp.fillRect(0, y, SCREEN_W, 14, HIGHLIGHT)
            disp.drawText(4, y + 2, "> " .. item, FG, HIGHLIGHT)
        else
            disp.drawText(4, y + 2, "  " .. item, FG, BG)
        end
    end
    
    local info_y = SCREEN_H - 30
    disp.drawText(2, info_y, "Provider: " .. provider_names[provider], disp.GRAY, BG)
    disp.drawText(2, info_y + 10, "Model: " .. model, disp.GRAY, BG)
end

local function draw_settings()
    local content_y = draw_header("SETTINGS") + 6
    
    local values = {
        provider_names[provider],
        api_endpoint,
        api_key ~= "" and "****" or "(none)",
        model,
        system_prompt:sub(1, 20) .. (system_prompt:len() > 20 and "..." or ""),
        "Back"
    }
    
    for i, item in ipairs(setting_items) do
        local y = content_y + i * 14
        if i == setting_selection then
            disp.fillRect(0, y, SCREEN_W, 14, HIGHLIGHT)
            disp.drawText(4, y + 2, "> " .. item .. ": " .. values[i], FG, HIGHLIGHT)
        else
            disp.drawText(4, y + 2, "  " .. item .. ": " .. values[i], FG, BG)
        end
    end
    
    pc.ui.drawFooter("[Ent] Edit  [Esc] Back", nil)
end

local function wrap_text(text, width)
    local lines = {}
    for line in text:gmatch("([^\n]*)\n?") do
        if #line == 0 then
            table.insert(lines, "")
        else
            while #line > width do
                local split = width
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

local function draw_chat()
    local content_y = draw_header("Chat: " .. model) + 1
    local line_height = 10
    
    local all_lines = {}
    for _, msg in ipairs(messages) do
        local wrapped = wrap_text(msg.content, COLS - 4)
        for _, line in ipairs(wrapped) do
            table.insert(all_lines, {text = line, role = msg.role})
        end
    end
    
    if is_streaming and accumulated_response ~= "" then
        local wrapped = wrap_text(accumulated_response, COLS - 4)
        for _, line in ipairs(wrapped) do
            table.insert(all_lines, {text = line, role = "assistant"})
        end
    end
    
    local visible_rows = math.floor((SCREEN_H - content_y - 17) / line_height)
    local max_scroll = math.max(0, #all_lines - visible_rows)
    scroll_y = math.min(max_scroll, scroll_y)
    
    for i = 1, visible_rows do
        local idx = scroll_y + i
        if idx <= #all_lines then
            local line = all_lines[idx]
            local y = content_y + (i - 1) * line_height
            local bg = line.role == "user" and USER_BG or ASSIST_BG
            disp.fillRect(2, y, SCREEN_W - 4, line_height - 1, bg)
            disp.drawText(4, y + 1, line.text, FG, bg)
        end
    end
    
    if is_streaming then
        pc.ui.drawFooter("[Esc] Cancel", "[Streaming...]")
    elseif api_key == "" then
        pc.ui.drawFooter("[Ent] Send  [Esc] Menu", "No API Key!")
    else
        pc.ui.drawFooter("[Ent] Send  [Esc] Menu", nil)
    end
end

-- ── Main Loop ─────────────────────────────────────────────────────────────────

local function main()
    load_config()
    
    while true do
        input.update()
        local pressed = input.getButtonsPressed()
        local char = input.getChar()
        
        if current_screen == SCR_WIFI_WAIT then
            if net.getStatus() == net.kStatusConnected then
                if not wifi_initialised then
                    wifi_initialised = true
                    current_screen = SCR_MENU
                end
            elseif net.getStatus() == net.kStatusNotAvailable then
                disp.clear(BG)
                draw_header("ERROR")
                disp.drawText(10, SCREEN_H // 2, "WiFi not available", ERROR_COL, BG)
                disp.flush()
                sys.sleep(2000)
                sys.exit()
            end
            if pressed & input.BTN_ESC ~= 0 then
                sys.exit()
            end
            
        elseif current_screen == SCR_MENU then
            if pressed & input.BTN_UP ~= 0 then
                menu_selection = math.max(1, menu_selection - 1)
            elseif pressed & input.BTN_DOWN ~= 0 then
                menu_selection = math.min(#menu_items, menu_selection + 1)
            elseif pressed & input.BTN_ENTER ~= 0 then
                if menu_selection == 1 then
                    if api_key == "" then
                        current_screen = SCR_SETTINGS
                        setting_selection = 3
                    else
                        current_screen = SCR_CHAT
                    end
                elseif menu_selection == 2 then
                    current_screen = SCR_SETTINGS
                    setting_selection = 1
                elseif menu_selection == 3 then
                    messages = {}
                    scroll_y = 0
                elseif menu_selection == 4 then
                    sys.exit()
                end
            end
            
        elseif current_screen == SCR_SETTINGS then
            if pressed & input.BTN_UP ~= 0 then
                setting_selection = math.max(1, setting_selection - 1)
            elseif pressed & input.BTN_DOWN ~= 0 then
                setting_selection = math.min(#setting_items, setting_selection + 1)
            elseif pressed & input.BTN_ENTER ~= 0 then
                if setting_selection == 1 then
                    provider = provider % 3 + 1
                    if provider == PROVIDER_ANTHROPIC then
                        model = "claude-3-haiku-20240307"
                    elseif provider == PROVIDER_OLLAMA then
                        model = "llama3"
                        api_endpoint = "http://192.168.1.1:11434/api/chat"
                    else
                        model = "gpt-4o-mini"
                        api_endpoint = "https://api.openai.com/v1/chat/completions"
                    end
                    save_config()
                elseif setting_selection == 2 then
                    local new_endpoint = pc.ui.textInput("API Endpoint:", api_endpoint)
                    if new_endpoint then
                        api_endpoint = new_endpoint
                        save_config()
                    end
                elseif setting_selection == 3 then
                    local new_key = pc.ui.textInput("API Key:", api_key)
                    if new_key then
                        api_key = new_key
                        save_config()
                    end
                elseif setting_selection == 4 then
                    local new_model = pc.ui.textInput("Model:", model)
                    if new_model then
                        model = new_model
                        save_config()
                    end
                elseif setting_selection == 5 then
                    local new_prompt = pc.ui.textInput("System Prompt:", system_prompt)
                    if new_prompt then
                        system_prompt = new_prompt
                        save_config()
                    end
                elseif setting_selection == 6 then
                    current_screen = SCR_MENU
                end
            elseif pressed & input.BTN_ESC ~= 0 then
                current_screen = SCR_MENU
            end
            
        elseif current_screen == SCR_CHAT then
            if is_streaming then
                if pressed & input.BTN_ESC ~= 0 and current_conn then
                    current_conn:close()
                    current_conn = nil
                    is_streaming = false
                end
            else
                if pressed & input.BTN_ESC ~= 0 then
                    current_screen = SCR_MENU
                elseif pressed & input.BTN_ENTER ~= 0 then
                    if api_key == "" then
                        if pc.ui.confirm("No API key configured. Open Settings to add one?") then
                            current_screen = SCR_SETTINGS
                            setting_selection = 3
                        end
                    else
                        local user_msg = pc.ui.textInput("Type message", "")
                        if user_msg and user_msg ~= "" then
                            table.insert(messages, {role = "user", content = user_msg})
                        
                        if provider == PROVIDER_ANTHROPIC then
                            table.insert(messages, {role = "assistant", content = ""})
                        end
                        
                        send_request(function(response, err)
                            if err then
                                table.insert(messages, {role = "assistant", content = "Error: " .. err})
                            else
                                if provider == PROVIDER_ANTHROPIC then
                                    messages[#messages].content = response
                                else
                                    table.insert(messages, {role = "assistant", content = response})
                                end
                            end
                            local total_lines = 0
                            for _, msg in ipairs(messages) do
                                local wrapped = wrap_text(msg.content, COLS - 4)
                                total_lines = total_lines + #wrapped
                            end
                            local vis_rows = math.floor((SCREEN_H - 47) / 10)
                            scroll_y = math.max(0, total_lines - vis_rows)
                        end)
                    end
                end
                end
                
                local all_lines = {}
                for _, msg in ipairs(messages) do
                    local wrapped = wrap_text(msg.content, COLS - 4)
                    for _, line in ipairs(wrapped) do
                        table.insert(all_lines, line)
                    end
                end
                local visible_rows = math.floor((SCREEN_H - 47) / 10)
                local max_scroll = math.max(0, #all_lines - visible_rows)
                if pressed & input.BTN_UP ~= 0 then
                    scroll_y = math.max(0, scroll_y - 3)
                elseif pressed & input.BTN_DOWN ~= 0 then
                    scroll_y = math.min(max_scroll, scroll_y + 3)
                end
            end
        end
        
        -- Drawing
        disp.clear(BG)
        if current_screen == SCR_WIFI_WAIT then
            draw_wifi_wait()
        elseif current_screen == SCR_MENU then
            draw_menu()
        elseif current_screen == SCR_SETTINGS then
            draw_settings()
        elseif current_screen == SCR_CHAT then
            draw_chat()
        end
        disp.flush()
        
        sys.sleep(16)
    end
end

main()
