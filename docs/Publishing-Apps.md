---
title: "Publishing Apps"
---

The App Store is an automatic index of public GitHub repositories tagged with
the topic `picodeck-app`. There is no registration or review: tag the repo,
publish a Release with one ZIP, and it is listed within 30 minutes.

The full contract and every rejection reason are documented on the store
itself. The [`app.json` reference](App-Json-Reference.md) has each field the
store reads.

- **Guide:** https://store.picodeck.net/publish
- **Why is my app not listed?** https://store.picodeck.net/status
- **Browse:** https://store.picodeck.net/
- **Catalog JSON the device reads:** https://store.picodeck.net/catalog.json

## Quick version

1. A public GitHub repo, not a fork, with `app.json` at the root. It needs `id`, `name` and `version`, all strings. The id must fit the store's rules, stricter than the firmware's: lower case, reverse-DNS, like `com.example.myapp` ([id rules](App-Json-Reference.md#id)).
2. Add the topic `picodeck-app` under the repo's About settings.
3. Bump `version` in `app.json`, then tag: the index reads `app.json` at the release's tag.
4. Create a Release with exactly one `.zip` whose root holds `app.json` and `main.lua` or `main.elf`, with no folder around them:

   ```sh
   cd myapp && zip -r ../myapp.zip .
   ```

5. The index computes the SHA-256 itself. Do not publish a checksum file.

The store installs to `/apps/<dirname>/`, which defaults to the id's last segment and is deleted and replaced on every update, so keep saved data under `/data`. A few names are reserved and each belongs to the first repository to use it ([`dirname`](App-Json-Reference.md#dirname)).

If you set [`min_firmware`](App-Json-Reference.md#min_firmware), use a released version: the store refuses to install an app that asks for a newer one than the device's firmware.

## Bundle the libraries you use

**Do not rely on `/system/lib` for an app you publish.** The libraries there (`download`, `panels`, `widgets`, the Spleen font) are plain files on the card, and nothing installs them: not the firmware, not a firmware update and not the store. Your own card has them because you copied them; other people's may not, and `picocalc.sys.loadlib("download")` then returns `nil, "cannot open /system/lib/download.lua"`.

Copy the library into your app instead, and load it with [`require`](Standard-Lua-Libraries.md#requirename), which looks in your app's folder first:

```
myapp/
├── app.json
├── main.lua
└── download.lua      copied from system/lib/ in the PicoDeck repo
```

```lua
local download = require("download")
```

Your copy also keeps working if a later version of the library changes. A font is the same: copy the `.pfn` into your app and load it with `picocalc.display.loadFont(APP_DIR .. "/spleen-5x8.pfn")`.

## Pointing a device at another indexer

From an app with the `"sysconfig"` requirement:

```lua
picocalc.sysconfig.set("store_url", "https://example.workers.dev/catalog.json")
picocalc.sysconfig.save()
```

To return to the default, remove the key the same way:

```lua
picocalc.sysconfig.set("store_url")
picocalc.sysconfig.save()
```
