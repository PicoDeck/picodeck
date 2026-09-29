---
title: "API — JSON"
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

```lua
local text = picocalc.json.encode({ name = "pico", score = 42 })
-- '{"name":"pico","score":42}'
```

---

#### `picocalc.json.decode(text)`
Decode a JSON string into Lua values. JSON `null` decodes to the `picocalc.json.null` sentinel (userdata), not `nil`.

- **Parameters:**
  - `text` (string): JSON text
- **Returns:** (any) Decoded value

```lua
local data = picocalc.json.decode('{"name":"pico","score":42}')
picocalc.sys.log(data.name)  -- "pico"
```

---

#### `picocalc.json.isNull(v)`
Check whether a value is JSON `null`, without referencing the sentinel directly.

- **Parameters:**
  - `v` (any): Value to test
- **Returns:** (boolean) `true` if `v` is the JSON `null` sentinel

```lua
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
