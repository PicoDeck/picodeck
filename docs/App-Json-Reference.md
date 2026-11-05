---
title: "app.json Reference"
---

`app.json` sits next to your `main.lua` or `main.elf`. Two different programs read it, and they read different fields:

- **The launcher**, in the firmware, reads it on the device every time it scans `/apps`. It uses a small, forgiving reader: a file that is not valid JSON still lists, and a value of the wrong type is ignored. It does not read the whole file as JSON, so keep every field at the top level.
- **The App Store index** reads it when you [publish](Publishing-Apps.md), on its own server, with a strict JSON parser. It copies some fields into the catalog that the App Store app on the device reads. On a device, the store only ever sees the catalog.

An app with no `app.json` still runs: the launcher lists it under its folder name, with the id `local.<folder>` and the version `?`.

```json
{
    "id": "com.example.myapp",
    "name": "My App",
    "version": "1.2.0",
    "description": "One line for the launcher and the store",
    "author": "Your Name",
    "category": "games",
    "requirements": ["audio"]
}
```

## Fields

| Field | Type | Default | Launcher | Store index | Valid values and limits |
|---|---|---|---|---|---|
| [`id`](#id) | string | `local.<folder>` | yes | **required** | See [below](#id) |
| `name` | string | the folder's name | yes | **required** | Launcher keeps 255 bytes, store 40 characters |
| `version` | string | `1.0` (launcher) | yes | **required** | A string: `1.0` as a number is `missing-field:version` in the store and ignored by the launcher. Launcher keeps 15 bytes, store 40 characters. The store compares the numbers in it to offer updates |
| `description` | string | empty | yes | yes (the repo's description) | Launcher keeps 127 bytes, store 120 characters |
| [`category`](#category) | string | `demos` | yes | yes | `games`, `tools`, `system`, `demos`, `emulators`, `network` |
| [`requirements`](#requirements) | array of strings | none | yes | shown in the store | See [App Requirements](Global-Variables-and-Permissions.md#app-requirements) |
| [`system_clock_khz`](#system_clock_khz) | integer | the OS's 200 MHz | yes | no | `125000`, `150000`, `200000`, `250000`, `300000` |
| [`min_psram_kb`](#min_psram_kb) | integer | no check | yes | no | KB |
| [`min_firmware`](#min_firmware) | string | `0.0.0` | no | yes | A released firmware version |
| `author` | string | the repo's owner | no | yes | 60 characters |
| `long_description` | string | empty | no | yes | 1000 characters, on the app's store page |
| `dirname` | string | the last segment of `id` | no | yes | [Below](#dirname) |
| `removable` | boolean | `true` | no | yes | `false` hides the store's Remove action |
| `homepage` | string | the repo's URL | no | yes | |
| `asset` | string | the only `.zip` | no | yes | The ZIP to use when a release has several |
| `icon` | string | none | no | yes | A path in your repo, for the store's web pages only. The launcher's icon is the file `icon.png` or `icon.bmp` beside `app.json` ([details](Creating-your-first-app.md#add-an-icon-optional)) |
| `screenshots` | array of strings | none | no | yes | Up to 4 paths in your repo, `.png`, `.jpg`, `.gif` or `.webp` |
| `keywords` | array of strings | none | no | yes | Up to 8, 24 characters each |

Nothing reads any other field. Some first-party apps carry `repo` and `type`, which have no effect: the launcher decides Lua or native by which file exists (`main.elf` wins), and the store takes the repository from GitHub.

The [store's guide](https://store.picodeck.net/publish) has the same store fields and every rejection reason.

## `id`

The id names your data folder, `/data/<id>/`, so changing it later leaves your users' saves behind. Set it before your first release. Two sets of rules apply, and the store's is stricter.

| | Launcher | Store index |
|---|---|---|
| Characters | `A-Z a-z 0-9 . _ -` | Lower case only: `^[a-z0-9]{1,32}(\.[a-z0-9_-]{1,32}){1,4}$` |
| Shape | No `..`, not `.`, no trailing `.` | 2 to 5 dot-separated segments of at most 32 characters; the first has no `_` or `-` |
| Length | 1 to 79 characters | Bounded by the shape |
| Case | Folded to lower case, so `com.Example.App` is `com.example.app` | Upper case is `bad-id` |
| If invalid | The launcher refuses to start the app, with a reason on screen and an entry in `/system/error.log` | The repository is not listed (`bad-id` on the [status page](https://store.picodeck.net/status)) |
| Uniqueness | None | First repository to publish an id owns it (`id-claimed-by:<repo>`) |

So write ids like `com.example.myapp`. `myapp`, `my_co.game` and `com.Example.App` all run on the device and are all refused by the store. The launcher refuses an id longer than 79 characters, with the reason on screen and in `/system/error.log`.

`APP_ID` holds the folded id.

## `category`

Picks the launcher's tab and the colour of the default icon. Upper or lower case is fine. A missing or unknown value means `demos`, and the store notes that on its status page.

## `requirements`

What each name does is on the [permissions page](Global-Variables-and-Permissions.md#app-requirements). Names are folded to lower case. Unknown names are ignored: there is no `"video"` or `"network"` requirement. The launcher keeps 127 bytes of the list.

## `system_clock_khz`

Runs your app at another CPU clock. The launcher switches before the app starts and goes back to 200 MHz when it exits. Any value not in the table above is ignored, with a line on the serial log, and the app runs at the default. Native emulators and heavy games use `300000`. A faster clock draws more power, and the panel's SPI clock is rescaled with it.

**WiFi:** without the `"http"` requirement, the launcher disconnects WiFi before changing the clock, and the app runs with no WiFi. The connection comes back after the app exits, unless the app or the user changed it meanwhile. With `"http"`, WiFi stays up through the change. So a networked app at a raised clock needs both fields.

The simulator accepts this field but does not change speed.

## `min_psram_kb`

Refuses the launch unless the PSRAM heap has one free block of at least this many KB. The largest block is the test, not the total free. The player sees `Not enough memory to launch:` with both figures, and `/system/error.log` gets an `APP FAILED` entry, instead of your app failing part-way through its start-up.

The whole heap is about 6 MB (6,160,384 bytes), shared with your code and stack if the app is native. The simulator's default allocator reports 8 MB, so test a value near the limit with `--real-umm`.

## `min_firmware`

The store only. The App Store app on the device compares it with `sys.getVersion()` and refuses to install a newer requirement than the firmware, saying `Requires firmware <version>+`. The launcher never reads it, so an app copied to the card by hand runs regardless.

Leave it out unless you know your minimum, and then use a version that has been released: the oldest firmware you have tested on, or the release that added the newest API you use. A value above the latest release makes the app impossible to install.

## `dirname`

Where the store installs your app: `/apps/<dirname>/`. It defaults to the last segment of the id, must be 1 to 32 characters of `A-Z a-z 0-9 _ -`, and belongs to the first repository to publish it (`dirname-claimed-by:<repo>`). The store **deletes that folder** before it extracts the new version, so an app must not keep data there: use `picocalc.fs.appPath`, which is under `/data`.

These are reserved, since PicoDeck ships them (`dirname-reserved`): `store`, `updater`, `filemanager`, `editor`, `terminal_example`, `calculator`, `system`, `data`, `.staging`. That rules out an id such as `com.example.store` unless you set another `dirname`.
