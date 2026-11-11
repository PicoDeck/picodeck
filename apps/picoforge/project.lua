-- project.lua — Project file management for PicoForge

local Project = {}
Project.__index = Project

local BASE_DIR = "/data/picoforge/projects"
local MAX_OPEN_BUFFERS = 8

function Project.new(name)
    return setmetatable({
        name = name,
        base_path = BASE_DIR .. "/" .. name,
        files = {},          -- list of relative paths
        buffers = {},        -- relative_path -> Buffer
        active_file = nil,   -- relative path of active file
        tab_size = 4,
        use_spaces = true,
    }, Project)
end

-- Ensure project directory structure exists
function Project:create(fs)
    fs.mkdir(BASE_DIR)
    fs.mkdir(self.base_path)
    -- Create default main.lua
    local main_path = self.base_path .. "/main.lua"
    if not fs.exists(main_path) then
        local f = fs.open(main_path, "w")
        if f then
            fs.write(f, '-- ' .. self.name .. '\n\nlocal pc = picocalc\nlocal disp = pc.display\nlocal input = pc.input\nlocal sys = pc.sys\n\ndisp.clear(0x0000)\ndisp.drawText(10, 10, "Hello from ' .. self.name .. '!", 0xFFFF)\ndisp.flush()\n\nwhile true do\n    input.update()\n    if input.justPressed("escape") then\n        return\n    end\n    sys.sleep(16)\nend\n')
            fs.close(f)
        end
    end
    -- Create project.json
    local cfg_path = self.base_path .. "/project.json"
    if not fs.exists(cfg_path) then
        local f = fs.open(cfg_path, "w")
        if f then
            fs.write(f, '{"name":"' .. self.name .. '","tab_size":4,"use_spaces":true}')
            fs.close(f)
        end
    end
end

-- Load project config
function Project:load_config(fs)
    local cfg_path = self.base_path .. "/project.json"
    if not fs.exists(cfg_path) then return end
    local f = fs.open(cfg_path, "r")
    if not f then return end
    local data = fs.read(f, 4096)
    fs.close(f)
    if not data then return end
    -- Simple JSON parse for known fields
    local tab_size = data:match('"tab_size"%s*:%s*(%d+)')
    local use_spaces = data:match('"use_spaces"%s*:%s*(true)')
    if tab_size then self.tab_size = tonumber(tab_size) end
    self.use_spaces = (use_spaces ~= nil)
end

-- Scan files in project directory
function Project:scan_files(fs)
    self.files = {}
    local entries = fs.listDir(self.base_path)
    if not entries then return end
    for _, entry in ipairs(entries) do
        if entry.name:match("%.lua$") then
            self.files[#self.files + 1] = entry.name
        end
    end
    table.sort(self.files)
end

-- Read a file into a buffer
function Project:open_file(fs, rel_path, Buffer)
    if self.buffers[rel_path] then
        self.active_file = rel_path
        return self.buffers[rel_path]
    end

    -- Check buffer limit
    local count = 0
    for _ in pairs(self.buffers) do count = count + 1 end
    if count >= MAX_OPEN_BUFFERS then
        return nil, "too many open buffers"
    end

    local path = self.base_path .. "/" .. rel_path
    local buf = Buffer.new(path)

    local f = fs.open(path, "r")
    if f then
        local chunks = {}
        while true do
            local data = fs.read(f, 4096)
            if not data or #data == 0 then break end
            chunks[#chunks + 1] = data
        end
        fs.close(f)
        buf:load_string(table.concat(chunks))
    end

    self.buffers[rel_path] = buf
    self.active_file = rel_path
    return buf
end

-- Close a buffer
function Project:close_file(rel_path)
    self.buffers[rel_path] = nil
    if self.active_file == rel_path then
        -- Switch to another open buffer
        self.active_file = nil
        for k in pairs(self.buffers) do
            self.active_file = k
            break
        end
    end
end

-- Get active buffer
function Project:get_active_buffer()
    if not self.active_file then return nil end
    return self.buffers[self.active_file]
end

-- Save a single buffer
function Project:save_file(fs, rel_path)
    local buf = self.buffers[rel_path]
    if not buf then return false end
    local path = self.base_path .. "/" .. rel_path
    local f = fs.open(path, "w")
    if not f then return false end
    local content = buf:to_string()
    fs.write(f, content)
    fs.close(f)
    buf.modified = false
    return true
end

-- Save all modified buffers
function Project:save_all(fs)
    for rel_path, buf in pairs(self.buffers) do
        if buf.modified then
            self:save_file(fs, rel_path)
        end
    end
end

-- Check if any buffer is modified
function Project:any_modified()
    for _, buf in pairs(self.buffers) do
        if buf.modified then return true end
    end
    return false
end

-- List all projects on SD card
function Project.list_projects(fs)
    local projects = {}
    local entries = fs.listDir(BASE_DIR)
    if not entries then return projects end
    for _, entry in ipairs(entries) do
        if entry.is_dir then
            -- Check for main.lua or project.json
            local has_main = fs.exists(BASE_DIR .. "/" .. entry.name .. "/main.lua")
            local has_config = fs.exists(BASE_DIR .. "/" .. entry.name .. "/project.json")
            if has_main or has_config then
                projects[#projects + 1] = entry.name
            end
        end
    end
    table.sort(projects)
    return projects
end

-- Scan functions across all open buffers (for autocomplete)
function Project:scan_all_functions()
    local all_funcs = {}
    local seen = {}
    for _, buf in pairs(self.buffers) do
        for _, name in ipairs(buf:scan_functions()) do
            if not seen[name] then
                seen[name] = true
                all_funcs[#all_funcs + 1] = name
            end
        end
    end
    table.sort(all_funcs)
    return all_funcs
end

return Project
