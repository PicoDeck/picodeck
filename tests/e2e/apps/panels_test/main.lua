-- E2E fixture for the panels.lua comics framework.
-- Text-only comic (no assets) sized so every state change needs only a
-- handful of key presses. The library's own "PANELS:" log markers plus the
-- render callbacks below are what the test asserts on.

local pc  = picocalc
local sys = pc.sys

local Panels = sys.loadlib("panels")
if type(Panels) ~= "table" then
    sys.log("PT NOLIB")
    return
end

local drew_marker = false

local comic = {
    name = "e2e",
    sequences = {
        -- 1 ── advance mode, two panels; layer text + renderFunction proof.
        {
            title = "Advance",
            scrollType = "advance",
            panels = {
                {
                    frame = { height = 320 },
                    layers = {
                        { text = "panel one", x = 20, y = 40 },
                        -- misspelled ease: logged once, animates linearly
                        { text = "slide", x = 20, y = 80,
                          animate = { x = 60, duration = 200, ease = "cubik" } },
                    },
                    renderFunction = function(panel, ox, oy, pct)
                        if not drew_marker then
                            drew_marker = true
                            sys.log("PT RENDERFN")
                        end
                    end,
                },
                {
                    frame = { height = 320 },
                    layers = { { text = "panel two", x = 20, y = 40 } },
                },
            },
        },
        -- 2 ── choice branch setting a story var.
        {
            title = "Choice",
            scrollType = "advance",
            panels = {
                {
                    frame = { height = 320 },
                    layers = { { text = "pick", x = 20, y = 40 } },
                    choices = {
                        prompt = "which?",
                        { text = "first",  target = 3, setVars = { picked = "a" } },
                        { text = "second", target = 3, setVars = { picked = "b" } },
                    },
                },
            },
        },
        -- 3 ── renderCondition resolves from the choice's var.
        {
            title = "Payoff",
            scrollType = "advance",
            panels = {
                {
                    frame = { height = 320 },
                    layers = {
                        { text = "got a", x = 20, y = 40,
                          renderCondition = { var = "picked", equals = "a" } },
                        { text = "got b", x = 20, y = 40,
                          renderCondition = { var = "picked", equals = "b" } },
                    },
                    updateFunction = function()
                        if not _G.__pt_payoff then
                            _G.__pt_payoff = true
                            sys.log("PT PAYOFF picked=" .. tostring(Panels.vars.picked))
                        end
                    end,
                },
            },
        },
    },
}

sys.log("PT START")
local result = Panels.start(comic, { resume = true })
sys.log("PT EXIT " .. tostring(result))
