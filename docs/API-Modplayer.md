---
title: "API Mod Player"
---

Tracker module music playback: **MOD files only**, the ProTracker family. Supported: modules tagged `M.K.`, `M!K!`, `FLT4`, `1CHN`–`9CHN` or `10CH`–`32CH` (1 to 32 channels), and untagged 15-sample Soundtracker modules. XM, S3M, IT and `FLT8` modules are not supported: `load` returns `false`. A file may be at most 512 KB.

The player renders the module on Core 1 at 22,050 Hz. Its output shares one PCM stream with the FilePlayer and `picocalc.audio.startStream`, so stop whichever of them is playing before you start another; starting one does not stop the others. SamplePlayers, tones and the MP3Player mix with it, so sound effects play over the music.

**Performance.** The player renders on the second core, but it reads the module's samples from PSRAM through the cache it shares with your app, so it slows your app's own code. Measured in a gfx3d game at 200 MHz with busy test modules (every channel starting a note on every row, 16 KB looping samples): a 4-channel module made each frame about 14% slower, an 8-channel one about 24%. A looping WAV through a FilePlayer cost 2.5%; 44.1 kHz stereo MP3 music cost nothing in a game that paces itself and about 21% in one that does not (see [Audio and Sound](API-Audio-and-Sound.md)). A module's whole soundtrack is tens of KB, against about 2.6 MB per minute of 22.05 kHz mono WAV.

## picocalc.modplayer

### Functions

#### `picocalc.modplayer.create()`
Create a new mod player instance.

- **Parameters:** None
- **Returns:** (userdata) Player object, or `nil, errorString` on failure

```lua
local player, err = picocalc.modplayer.create()
if not player then
    picocalc.repl.print("Error: " .. err)
end
```

---

### Player Methods

Objects returned by `picocalc.modplayer.create()`. There is one MOD player:
`create()` returns the one live handle (calling it again returns the same
object while it is alive). It is cleaned up by the garbage collector; a
destroyed handle raises on use. Volume is 0-100.

#### `player:load(path)`
Load a tracker module file from the SD card.

- **Parameters:**
  - `path` (string): Path to a MOD file
- **Returns:** (boolean) `true` if the file was loaded; `false` if it is missing, larger than 512 KB, or not a MOD file the player can read

```lua
local ok = player:load("/apps/myapp/music.mod")
```

---

#### `player:play([loop])`
Start playback of the loaded module.

- **Parameters:**
  - `loop` (boolean, optional): Whether to loop playback. Default `false`.
- **Returns:** None

```lua
player:play(true)  -- play with looping
```

---

#### `player:stop()`
Stop playback.

- **Parameters:** None
- **Returns:** None

```lua
player:stop()
```

---

#### `player:pause()`
Pause playback at the current position.

- **Parameters:** None
- **Returns:** None

```lua
player:pause()
```

---

#### `player:resume()`
Resume paused playback.

- **Parameters:** None
- **Returns:** None

```lua
player:resume()
```

---

#### `player:isPlaying()`
Check whether the player is currently playing.

- **Parameters:** None
- **Returns:** (boolean) `true` if playback is active

```lua
if player:isPlaying() then
    picocalc.repl.print("Playing...")
end
```

---

#### `player:setVolume(vol)`
Set the playback volume.

- **Parameters:**
  - `vol` (number): Volume level from 0 (silent) to 100 (maximum)
- **Returns:** None

```lua
player:setVolume(75)
```

---

#### `player:getVolume()`
Get the current playback volume.

- **Parameters:** None
- **Returns:** (number) Current volume level (0-100)

```lua
local vol = player:getVolume()
```

---

#### `player:setLoop(enabled)`
Enable or disable looping on the current module.

- **Parameters:**
  - `enabled` (boolean): `true` to loop, `false` to play once
- **Returns:** None

```lua
player:setLoop(true)
```
