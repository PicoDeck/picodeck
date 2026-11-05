---
title: "Getting started on a device"
---

This page takes a PicoCalc from the box to a running app. To try PicoDeck first without any hardware, use the [simulator](Simulator-and-Testing.md) or [run it in your browser](https://picodeck.net/try/).

## What you need

- A ClockworkPi [PicoCalc](https://www.clockworkpi.com/picocalc).
- A [Pimoroni Pico Plus 2 W](https://shop.pimoroni.com/products/pimoroni-pico-plus-2-w) fitted in it, **in place of the Pico board the PicoCalc comes with**. This is the only board release firmware is built for: PicoDeck keeps its Lua heap and native apps in that board's 8 MB of PSRAM, which other Pico boards do not have.
- A microSD card, formatted as **FAT32**. Cards over 32 GB often come formatted as exFAT, so reformat them.
- A USB cable, and a computer to copy files from.

## 1. Install the firmware

Download `picodeck.uf2` from the [latest release](https://github.com/PicoDeck/picodeck/releases/latest), or [build it yourself](https://github.com/PicoDeck/picodeck#build-setup).

1. Connect nothing yet. Hold the **BOOTSEL** button on the Pico Plus 2 W.
2. While holding it, connect the board's own USB port to your computer, then release the button.
3. A USB drive named `RP2350` appears. Copy `picodeck.uf2` onto it.
4. The drive disappears and the board restarts by itself.

USB powers only the Pico board. The screen and keyboard run from the PicoCalc's own batteries and power switch, so switch it on to see anything. With no card in the slot, the screen then says `SD card not found! Insert a FAT32 SD card and press Enter to retry.`

## 2. Put apps on the card

Flashing the firmware puts nothing on the card, and **no apps are built into the firmware**. A blank card boots to an empty launcher that says `No apps in this category.` There is no App Store on it either: the store is itself an app on the card. So the first step is to copy some apps over by hand.

From the same release page, download **Source code (zip)** and unpack it. Copy two of its folders to the root of the card:

- `apps/`: the first-party apps, the App Store and System Update among them.
- `system/`: it holds `lib/`, the shared Lua libraries. Nothing else installs these, and some first-party apps need them.

The card then looks like this. `/data` and any missing top-level folders are created when the card is mounted.

```
SD:/
├── apps/
│   ├── store/          the App Store
│   ├── updater/        System Update
│   ├── hello/
│   │   ├── app.json
│   │   └── main.lua
│   └── ...
├── data/               each app's saved data, created as needed
└── system/
    └── lib/            download.lua, panels.lua, widgets.lua, fonts/
```

You can copy less: `store/` alone gets you the App Store. The launcher lists at most 64 apps. `apps/gbc` has no program until you build it, so it does not appear.

## 3. First boot

Put the card in and power on. After the boot animation the launcher lists the apps by category.

| Key | At the launcher |
|---|---|
| Up, Down | Choose an app |
| Left, Right | Change the category tab |
| Enter | Launch the app |
| Shift+F5 | Open the system menu |

The system menu key is **Shift+F5**, which the keyboard reports as F10, so the launcher's footer shows `F10:Menu`. It opens the menu from any app, and pressing it again closes the menu. In an app, Esc usually quits, but that is up to the app; the system menu's **Exit App** always works.

## 4. Connect to WiFi

The App Store and any networked app need WiFi. In the system menu open **Settings**, then **WiFi Settings**, and type the network name and password. They are saved in `/system/config.json` on the card, unencrypted. The device connects at boot to set its clock, then disconnects again unless the running app declares the [`"http"` requirement](Global-Variables-and-Permissions.md#app-requirements) or you turn off **Auto Disconnect** in the same menu.

## 5. Copy your own apps

An app is a folder at `/apps/<name>/`; [Creating your first app](Creating-your-first-app.md) writes one. There are three ways to get it onto the card.

**USB Disk Mode** is the usual way, with no need to take the card out.

1. Connect the board's USB port to your computer.
2. At the launcher, open the system menu and choose **USB Disk Mode**. It is only offered at the launcher, not while an app runs. The screen says `USB Mode`, and the card appears as a drive on your computer.
3. Copy your folder into `apps/`, then eject the drive; that ends the mode. **Holding Esc** on the keyboard ends it too, and takes the disk away from the computer at once, so eject first.

The launcher reads the card again when the mode ends, so a new app is listed straight away. The mode also ends by itself if no computer has connected within about 10 seconds. WiFi is disconnected during it and comes back after.

**A card reader** also works: power off, move the card, copy, move it back, power on.

**The App Store** installs anything [published to it](Publishing-Apps.md): open App Store from the launcher (it needs WiFi). Installed apps go to `/apps/<dirname>/`.

For repeated copies while developing, the [dev commands](Simulator-and-Testing.md#dev-commands) over USB serial can upload a whole app with no need for USB Disk Mode.

## 6. Keep it up to date

**System Update** (`apps/updater`) and the App Store install new firmware over WiFi, signed releases only. Updating the firmware does not touch the card: to get newer first-party apps or libraries, copy `apps/` and `system/` from the new release's source zip, or update what the App Store lists.

To flash by hand again without reaching for BOOTSEL, turn on **Settings > Developer Mode**; the system menu then has **Reboot to Flash**, which brings up the `RP2350` drive.

## If something goes wrong

| What you see | What it means |
|---|---|
| A black screen, though the computer sees the board | The PicoCalc is switched off and USB is powering only the Pico. Switch it on and reboot. |
| `SD card not found!` | No card, or it is not FAT32. Press Enter to retry once it is in. |
| `No apps in this category.` | Nothing usable in that tab. On every tab: the card is blank, or the apps are not at `/apps/<name>/`. A folder needs a `main.lua` or `main.elf` to be listed. |
| A new app is not listed | The launcher reads `/apps` at boot, when an app exits, and after USB Disk Mode. Reboot from the system menu. |
| `panels library not found` (Panels Demo), or `loadlib` returns `nil, "cannot open /system/lib/..."` | `system/lib` is not on the card. Copy it. |
| `Cannot launch app: invalid app id in app.json` | See the [id rules](App-Json-Reference.md#id). |
| An error screen in your app | Press Esc. The same text is in `/system/error.log` on the card. See [Crash Logging and Watchdog](Crash-Logging-and-Watchdog.md), which also covers `Boot failed N times - halted`. |
