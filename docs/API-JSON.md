---
title: "API JSON"
---

JSON encoding and decoding. JSON `null` maps to a sentinel value, **not** `nil` — a `nil` table value would delete the key.

## picocalc.json

### Functions

#### `picocalc.json.encode(value [, opts])`
Encode a Lua value as a JSON string.

- **Parameters:**
  - `value` (string | number | boolean | table): Value to encode. Use `picocalc.json.null` to emit JSON `null`.
  - `opts` (table, optional): `{indent = n}` pretty-prints with `n` spaces per level (default 0: compact)
- **Returns:** (string) JSON text
- **Errors:** raises on a function, userdata other than `picocalc.json.null`, a thread, NaN or infinity, a table that contains itself, and nesting deeper than 32 levels. Wrap the call in `pcall` if the data is not known to be safe.

```lua
local text = picocalc.json.encode({ name = "pico", score = 42 })
-- '{"name":"pico","score":42}'
```

Lua tables and JSON do not line up exactly, so some shapes change without an error:

| Lua value | JSON |
|---|---|
| An empty table `{}`, also one decoded from `[]` | `{}`: an empty array comes back as an empty object |
| A sequence `{1, 2, 3}` | `[1,2,3]` |
| A table with holes, or a mix: `{1, 2, x = 3}` | An object: `{"1":1,"2":2,"x":3}` |
| A number as a key | Its string: `{[10] = "a"}` gives `{"10":"a"}` |
| A boolean as a key: `{[true] = 1}` | Dropped: `{}` |

---

#### `picocalc.json.decode(text)`
Decode a JSON string into Lua values. JSON `null` decodes to the `picocalc.json.null` sentinel (userdata), not `nil`.

- **Parameters:**
  - `text` (string): JSON text
- **Returns:** (any) Decoded value, or `nil, errorString` for invalid input (it does not raise). That covers text after the value and nesting deeper than 32 levels. A message reads `json.decode: … at byte N`.

Numbers follow Lua's: a whole number that fits in 32 bits is an integer, and anything larger becomes a float, good to about 7 significant digits (`123456789012` decodes as `1.234568e+11`). Keep large ids as strings.

```lua
local data, err = picocalc.json.decode('{"name":"pico","score":42}')
if not data then
    picocalc.sys.log("bad JSON: " .. err)
    return
end
picocalc.sys.log(data.name)  -- "pico"
```

---

#### `picocalc.json.isNull(v)`
Check whether a value is JSON `null`, without referencing the sentinel directly.

- **Parameters:**
  - `v` (any): Value to test
- **Returns:** (boolean) `true` if `v` is the JSON `null` sentinel

```lua
local data = picocalc.json.decode('{"middle_name":null}')
if picocalc.json.isNull(data.middle_name) then
    -- field is explicitly null
end
```

---

### Constants

#### `picocalc.json.null`
Sentinel value representing JSON `null`. Encode it to emit `null`; decoded `null` fields compare equal to it.

```lua
local text = picocalc.json.encode({ value = picocalc.json.null })
-- '{"value":null}'
```

---

### Example

Decoding an API response and checking for null fields.

```lua
local body = '{"name":"pico","avatar_url":null,"score":1200}'
local user = picocalc.json.decode(body)

if picocalc.json.isNull(user.avatar_url) then
    picocalc.sys.log(user.name .. " has no avatar")
else
    picocalc.sys.log("avatar: " .. user.avatar_url)
end
```
