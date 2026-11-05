# PicoDeck SDK

What an app author takes from this repository. To write a Lua app you need nothing from here but a text editor; start with [Creating your first app](https://picodeck.net/docs/creating-your-first-app/). The rest of the docs are at [picodeck.net/docs](https://picodeck.net/docs/).

```
sdk/
├── lua/picocalc.lua    Type stubs for your editor (never runs on the device)
├── native/             Template for a native (C) app: headers, linker script, Makefile, sample
└── fonts/spleen/       Source of the Spleen font in system/lib/fonts/
```

A few other things an author may want are elsewhere in the repository:

- [`system/lib/`](../system/lib): the shared Lua libraries (`download`, `panels`, `widgets`) and a font. Nothing installs them on a user's card, so [bundle what you publish](https://picodeck.net/docs/publishing-apps/#bundle-the-libraries-you-use).
- [`tools/mkfont.py`](../tools/mkfont.py): converts a font to the `.pfn` format that `display.loadFont` reads.
- [`apps/`](../apps): the first-party apps; the docs list the [ones to read first](https://picodeck.net/docs/creating-your-first-app/#sample-apps).

## Editor setup for Lua

`lua/picocalc.lua` gives [lua-language-server](https://luals.github.io/) (LuaLS; the "Lua" extension in VS Code) completion, hover docs and argument checks for the whole `picocalc` API. Copy it somewhere your app's project can see it, and put a `.luarc.json` at the project's root:

```json
{
  "runtime.version": "Lua 5.4",
  "workspace.library": ["libs/picocalc.lua"],
  "diagnostics.globals": ["APP_DIR", "APP_NAME", "APP_ID", "APP_REQUIREMENTS"]
}
```

A path in `workspace.library` is relative to your project (an absolute path into a PicoDeck checkout works too). The `["sdk/lua"]` in this repository's own `.luarc.json` only means something inside this repository.

The stub does not hide what PicoDeck leaves out of Lua: `io`, `os`, `package` and `debug` still complete in your editor and are `nil` on the device. See [Standard Lua Libraries](https://picodeck.net/docs/standard-lua-libraries/).

## Native apps

Copy the whole of `native/`, since `os.h` includes `terminal.h`. You need `arm-none-eabi-gcc` (Fedora: `arm-none-eabi-gcc-cs`; Debian/Ubuntu: `gcc-arm-none-eabi`).

```sh
cp -r sdk/native myapp
cd myapp
make
```

That gives `main.elf`. Edit `app.json` (the [`app.json` reference](https://picodeck.net/docs/app-json-reference/) has every field), then copy `main.elf` and `app.json` to `/apps/<dir>/` on the card, or on a [simulator card](https://picodeck.net/docs/simulator-and-testing/#the-sd-card-is-a-folder). [Native App Development](https://picodeck.net/docs/native-loading/) has the rest.
