---
title: "API Audio and Sound"
---

Audio output: tones, samples, streamed files, MP3, tracker modules and a raw PCM stream. One mixer sums everything into a single 44.1 kHz stereo output, so different kinds of sound play at once, apart from the conflict described under [The shared PCM stream](#the-shared-pcm-stream).

## Which player to use

| You want | Use | Takes | How many | Bit depth in the mix |
|---|---|---|---|---|
| Sound effects | [`Sample`](#sample) with `sample:play()`, or a [SamplePlayer](#sampleplayer) | WAV, 8- or 16-bit, up to 64 KB (0.37 s of 44.1 kHz stereo, 1.5 s of 22.05 kHz mono) | 8 voices play at once | 16 |
| Music from the SD card | [FilePlayer](#fileplayer) | QOA, or WAV 16-bit | 2 can exist, 1 plays | 8 |
| Music from the SD card, 16-bit | [MP3Player](#mp3player) | MP3 | 1 | 16 |
| Small music files | [MOD player](API-Modplayer.md) | MOD, up to 512 KB | 1 | 8 (rendered at 22.05 kHz) |
| A beep | [`audio.playTone`](#functions) | A frequency: square wave | 1 tone | |
| Sound you generate | [`audio.startStream`](#pcm-streaming) and `pushSamples` | 16-bit values | 1 stream | 8 |

For music in a real-time game, the figures under **Performance** in [MP3Player](#mp3player) decide between MP3 and QOA.

The FilePlayer, the MOD player and `pushSamples` go through the PCM stream, which keeps 8 bits per channel whatever goes in. SamplePlayers and the MP3Player mix at 16 bits.

The four players differ in shape:

| | SamplePlayer | FilePlayer | MP3Player | MOD player |
|---|---|---|---|---|
| Create | `sound.sampleplayer()` | `sound.fileplayer()` | `sound.mp3player()` | `modplayer.create()` |
| Failed load | `nil, errstr` | `nil, errstr` | `nil, errstr` | `false` |
| Loop | `play(0)` | `play(0)`, `setLoopRange` | `play(0)`, `setLoop` | `play(true)`, `setLoop` |
| Pause | `setPaused(bool)` | `pause`, `resume` | `pause`, `resume` | `pause`, `resume` |
| Position | `getOffset`, `setOffset`: seconds, fractions allowed | `getOffset`, `setOffset`: whole seconds | `getPosition`: frames; no seek | none |
| Play part of it | `setPlayRange`: frames | `setLoopRange`: whole seconds | no | no |
| Told when it ends | `setFinishCallback`, `setLoopCallback` | `setFinishCallback`, `setLoopCallback` | no: poll `isPlaying` | no: poll `isPlaying` |
| Volume | `setVolume(v)` | `setVolume(left [, right])`, `right` ignored | `setVolume(v)` | `setVolume(v)` |

---

## picocalc.audio

Tones, the master volume and a raw PCM stream.

### The shared PCM stream

The FilePlayer, the MOD player and `startStream` share one PCM stream, with one buffer and one sample rate. Only one of them may play at a time, so stop the one that is playing before starting another. The only automatic stop is that starting a FilePlayer stops another FilePlayer. Tones, SamplePlayers and the MP3Player mix with the stream and with each other, so they all play at once.

Breaking the rule raises no error and reports nothing:

| You do | What happens |
|---|---|
| Start a second user: `mod:play()` under a FilePlayer, `fp:play()` under a MOD, `startStream` under either | The buffer is emptied and the stream takes the new rate. Both keep pushing into it, so both are garbled, and each runs at the wrong speed: a 22.05 kHz FilePlayer runs at twice its speed under `startStream(44100)`. Both still report `isPlaying()` |
| Stop one of the two: `mod:stop()`, `fp:stop()` | The stream stops for both. The other falls silent, but `isPlaying()` stays `true`, its position stops moving, `didUnderrun()` is `false` and `ringFree()` is `0`. It stays that way until something starts the stream again |
| `stopStream()` under a FilePlayer or a MOD | The same: silent, still "playing", waiting. A later `startStream` lets it go on, at that stream's rate |

### Functions

#### `picocalc.audio.playTone(freq [, duration])`
Plays a square wave at the specified frequency. There is one tone: a new call replaces the one playing.

- **Parameters:**
  - `freq` (number): Frequency in Hz (e.g., `440` for concert A), held to 20-20000
  - `duration` (number, optional): Duration in milliseconds. If `0` or omitted, the tone plays indefinitely until `stopTone()` is called.
- **Returns:** None

```lua
picocalc.audio.playTone(440, 200)   -- beep for 200ms
picocalc.audio.playTone(880)        -- start continuous tone
```

---

#### `picocalc.audio.stopTone()`
Stops any currently playing tone immediately.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.audio.stopTone()
```

---

#### `picocalc.audio.setVolume(volume)`
Sets the master volume. It scales everything the mixer plays: tones, SamplePlayers, the stream (and so the FilePlayer and the MOD player) and the MP3Player. The master volume resets to 100 when the app exits.

Everything but the tone scales in proportion. The tone falls much faster, because its level follows a curve of the master volume and the mix is then scaled by the master volume again (at 100 a tone is half of full scale):

| `setVolume` | Samples, stream, MP3 | Tone |
|---|---|---|
| 100 | 100% | 100% |
| 75 | 75% | 39% |
| 50 | 50% | 12% |
| 25 | 25% | 2% |
| 10 | 10% | 0.3% |

- **Parameters:**
  - `volume` (number): Volume level (0–100, where 0 is muted and 100 is maximum; larger values clamp to 100)
- **Returns:** None

```lua
picocalc.audio.setVolume(50)  -- half volume for everything but a tone
```

---

### PCM Streaming

#### `picocalc.audio.startStream(sampleRate)`
Starts (or restarts) the PCM stream at `sampleRate` and empties its buffer. Other sounds keep playing. The FilePlayer and the MOD player use this stream while they play. The stream plays from the first samples you push: until then it is silent, and that silence does not count as an underrun.

- **Parameters:**
  - `sampleRate` (number): Sample rate in Hz (e.g. `44100`)
- **Returns:** None

```lua
picocalc.audio.startStream(44100)
```

---

#### `picocalc.audio.stopStream()`
Stops the PCM stream and empties its buffer. Tones and samples keep playing.

- **Parameters:** None
- **Returns:** None

```lua
picocalc.audio.stopStream()
```

---

#### `picocalc.audio.pushSamples(samples)`
Push audio samples to the streaming buffer. Samples are interleaved stereo pairs (left, right, left, right...). Only while a stream is started (`startStream`): pushes before it, or after `stopStream`, are dropped. Frames that do not fit the buffer are dropped too, so check `ringFree()` first. The values are 16-bit, but the stream keeps the top 8 bits of each.

- **Parameters:**
  - `samples` (table): Array of int16 sample values. At most 512 values (256 stereo frames) are taken from one call, and an odd last value is ignored.
- **Returns:** None

```lua
local samples = {}
for i = 1, 512 do
    samples[i] = math.floor(math.sin(i * 0.1) * 16000)
end
picocalc.audio.pushSamples(samples)
```

---

#### `picocalc.audio.ringFree()`
Get the room left in the stream's buffer, in stereo frames: 4096 when empty. One frame is two values in `pushSamples`. While no stream is started it reports 0 (pushes are dropped then).

- **Parameters:** None
- **Returns:** (number) Free stereo frames

```lua
local samples = {}
for i = 1, 512 do samples[i] = 0 end   -- 512 values: 256 frames

if picocalc.audio.ringFree() >= #samples // 2 then
    picocalc.audio.pushSamples(samples)
end
```

---

## picocalc.sound

Full audio playback system supporting WAV samples and MP3 files. Provides three player types:
- **SamplePlayer** — plays a pre-loaded WAV sample from memory
- **FilePlayer** — streams a WAV or QOA file from the SD card
- **MP3Player** — streams an MP3 file from the SD card

### Repeat counts

`play(repeat)` takes a count, and the players read it differently:

| Player | `play(0)` | `play(n)`, n ≥ 1 |
|---|---|---|
| SamplePlayer | loops until stopped | plays `n` times |
| FilePlayer | loops until stopped | plays `n` times |
| MP3Player | loops until stopped | plays once (the count is ignored) |

Omitting the argument is `1` everywhere. The C calls `playerPlay`, `filePlayerPlay` and `mp3PlayerPlay` behave the same way.

### Top-Level Functions

#### `picocalc.sound.getCurrentTime()`
Returns the audio clock: milliseconds of audio output since the last `resetTime()`, or since the app started. It counts what the output has played, so it advances only while the output runs: it reads `0` until the app's first sound of any kind, and from then on it keeps running, through silence too, until the app exits. It wraps to `0` after about 27 hours.

- **Returns:** (number) Milliseconds, a whole number

---

#### `picocalc.sound.resetTime()`
Resets the audio clock (`getCurrentTime()`) to zero.

- **Returns:** None

---

#### `picocalc.sound.playingSources()`
Returns how many SamplePlayers are playing. A FilePlayer, MP3Player, MOD player, tone or stream does not count.

- **Returns:** (number) Sample voices in use

```lua
local n = picocalc.sound.playingSources()
picocalc.sys.log("Active sources: " .. n)
```

---

### Sample

A `Sample` holds raw PCM audio data loaded from a WAV file.

#### `picocalc.sound.sample([path])`
Creates a new Sample object, optionally loading a WAV file immediately.

WAVs must be 8- or 16-bit PCM with 1-2 channels (float, 24/32-bit, ADPCM and more channels are refused); only the first 64 KB of sample data is kept.

Samples have no fixed limit: each lives in PSRAM until it is collected.

- **Parameters:**
  - `path` (string, optional): Absolute path to a WAV file. Without it the Sample is empty until `sample:load()`: it has no length and does not play.
- **Returns:** (userdata) Sample object, or `nil, errstr` on failure

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
```

---

#### `picocalc.sound.sample(seconds [, format])`
Creates a silent Sample of that length: a placeholder. No API writes into a Sample, so it can only be played as silence, saved, or replaced with `sample:load()`. The data is capped at 64 KB, which `getLength()` then reports.

- **Parameters:**
  - `seconds` (number): Length in seconds
  - `format` (table, optional): `sampleRate` (default `44100`), `bits` (`8` or `16`, default `16`), `channels` (`1` or `2`, default `1`)
- **Returns:** (userdata) Sample object, or `nil, errstr` on failure

```lua
local gap = picocalc.sound.sample(0.25, { sampleRate = 22050 })
```

---

#### `sample:load(path)`
Loads a WAV file into the sample.

WAVs must be 8- or 16-bit PCM with 1-2 channels (float, 24/32-bit, ADPCM and more channels are refused); only the first 64 KB of sample data is kept.

- **Parameters:**
  - `path` (string): Absolute path to a WAV file
- **Returns:** `true` on success, or `nil, errstr` on failure

---

#### `sample:getLength()`
Returns the number of PCM samples (frames).

- **Returns:** (number)

---

#### `sample:getSampleRate()`
Returns the sample rate in Hz (e.g., `44100`).

- **Returns:** (number)

---

#### `sample:getFormat()`
Returns the audio format of the sample as a table.

- **Returns:** (table) With fields:
  - `bits` (number): Bits per sample (e.g. 8, 16)
  - `channels` (number): Number of channels (1=mono, 2=stereo)
  - `sampleRate` (number): Sample rate in Hz

```lua
local fmt = sample:getFormat()
picocalc.sys.log(fmt.bits .. "bit, " .. fmt.channels .. "ch, " .. fmt.sampleRate .. "Hz")
```

---

#### `sample:decompress()`
Returns the sample itself (no-op). Provided for API compatibility with engines that distinguish compressed and decompressed sample data. On PicoDeck, samples are always stored decompressed.

- **Returns:** (userdata) The same Sample object

---

#### `sample:getSubsample(start, end)`
Creates a new Sample containing a slice of the original sample's PCM data.

- **Parameters:**
  - `start` (number): Start offset in PCM frames
  - `end` (number): End offset in PCM frames, not included. Past the end of the sample it means the end.
- **Returns:** (userdata) New Sample object, or `nil, errstr` if the slice is empty or memory runs out

```lua
local clip = sample:getSubsample(0, 22050)  -- the first half second at 44100 Hz
```

---

#### `sample:play([repeatCount [, rate]])`
Plays the sample on a temporary SamplePlayer and returns the player, which you are free to drop. This is the simple way to play a sound effect (see [Playing sound effects](#playing-sound-effects)).

- While it plays, a temporary player is safe from the garbage collector with or without your reference, and it keeps the Sample alive, so you may drop that too.
- When it finishes or you stop it, its voice is free again within a few milliseconds, even if you kept the player. A player you kept goes on working: `isPlaying()` is `false`, the getters and setters still answer, and `play()` takes a voice again.
- A looping one (`repeatCount` `0`) plays until you stop it, so keep the player: nothing else can stop it before the app exits.
- With all 8 voices taken it returns `nil, errstr` and plays nothing.

- **Parameters:**
  - `repeatCount` (number, optional): Number of times to play, `0` to loop until stopped (default `1`)
  - `rate` (number, optional): Playback rate multiplier (default `1.0`)
- **Returns:** (userdata) SamplePlayer object, or `nil, errstr` with no voice free or no data in the sample

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
s:play()           -- play once at normal speed
s:play(3, 1.5)     -- play 3 times at 150% speed
```

---

#### `sample:playAt(when [, vol [, rightvol [, rate]]])`
Plays the sample once on a temporary SamplePlayer, as `sample:play()` does, with a volume. The `when` parameter is accepted for API compatibility but ignored on this hardware (playback starts immediately).

- **Parameters:**
  - `when` (number): Ignored (accepted for API compatibility)
  - `vol` (number, optional): Volume 0–100 (default `100`; larger values clamp)
  - `rightvol` (number, optional): Ignored. The output is stereo, but a player has one volume: `vol` sets both channels, and there is no panning.
  - `rate` (number, optional): Playback rate multiplier (default `1.0`)
- **Returns:** (userdata) SamplePlayer object, or `nil, errstr` with no voice free or no data in the sample

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
s:playAt(0, 60)          -- play at volume 60
s:playAt(0, 100, 0, 2.0) -- play at full volume and double speed
```

---

#### `sample:save(filename)`
Writes the sample data to a WAV file on the SD card.

- **Parameters:**
  - `filename` (string): Path to write
- **Returns:** `true` on success, or `false, errstr` on failure

```lua
local clip = sample:getSubsample(0, 22050)
clip:save(picocalc.fs.appPath("clip.wav"))
```

---

### Playing sound effects

Load each effect once into a Sample, keep the Sample, and call `play`. Drop what it returns: the sound plays to the end whatever the garbage collector does, and gives its voice back when it ends.

```lua
local shot = picocalc.sound.sample(APP_DIR .. "/shot.wav")
local boom = picocalc.sound.sample(APP_DIR .. "/boom.wav")

shot:play()          -- in the game loop, as often as it fires
boom:playAt(0, 60)   -- at volume 60
```

The mixer has 8 voices. With all of them taken `play` returns `nil, errstr` and that sound is skipped, which is usually what a game wants. To keep voices for the sounds that matter, or to allocate nothing per shot, hold a pool of SamplePlayers: each keeps its voice for as long as it exists, and `setSample` makes it play anything.

```lua
local shot = picocalc.sound.sample(APP_DIR .. "/shot.wav")
local boom = picocalc.sound.sample(APP_DIR .. "/boom.wav")

local pool, nextVoice = {}, 1
for i = 1, 4 do pool[i] = picocalc.sound.sampleplayer() end

local function sfx(sample, volume)
    local p = pool[nextVoice]
    nextVoice = nextVoice % #pool + 1
    p:setSample(sample)        -- cuts off whatever this voice was playing
    p:setVolume(volume or 100)
    p:play()
end

sfx(shot)
sfx(boom, 60)
```

---

### SamplePlayer

Plays a `Sample` from memory. Supports looping and volume control.

#### `picocalc.sound.sampleplayer([sample_or_path])`
Creates a SamplePlayer, optionally pre-loading a sample.

A SamplePlayer keeps its Sample alive (you may drop your own reference). `sampleplayer(path)` creates a Sample of its own. Unlike the temporary players of `sample:play()`, a SamplePlayer needs your reference: dropped, it stops when it is collected, playing or not.

A SamplePlayer holds one of the mixer's 8 voices for as long as it exists, whether it plays or not. With every voice taken, a 9th returns `nil, errstr`, after a collection in case some of them are garbage. Reuse players and call `setSample`, or keep a pool (see [Playing sound effects](#playing-sound-effects)).

- **Parameters:**
  - `sample_or_path` (userdata or string, optional): A `Sample` object or a WAV file path
- **Returns:** (userdata) SamplePlayer object, or `nil, errstr` on failure

```lua
local player = picocalc.sound.sampleplayer("/apps/myapp/beep.wav")
player:play()
```

---

#### `player:setSample(sample)`
Sets the sample to play, stopping nothing: a playing player goes on from the start of the new sample.

- **Parameters:**
  - `sample` (userdata): A `Sample` object. Anything else raises.
- **Returns:** `true`

---

#### `player:play([repeat])`
Starts playback from the beginning of the play range.

- **Parameters:**
  - `repeat` (number, optional): Number of times to play. `0` loops until stopped. Default `1`.
- **Returns:** `true` if started, or `nil, errstr` if there is nothing to play (no sample, or an empty one), or for a finished player from `sample:play()`, which has given its voice back, if no voice is free

---

#### `player:stop()`
Stops playback.

---

#### `player:isPlaying()`
- **Returns:** (boolean)

---

#### `player:setVolume(vol)` / `player:getVolume()`
Volume range 0–100 (larger values clamp to 100).

---

#### `player:getSample()`
Returns the Sample object currently assigned to this player.

- **Returns:** (userdata) the Sample object, or `nil` if no sample is set

---

#### `player:setPaused(paused)`
Pauses or unpauses playback without resetting the playback position. A player from `sample:play()` gives its voice back while paused (dropped then, it is collected and the sound is gone), so resuming it can find none free: `setPaused(false)` then returns `nil, errstr` and it stays paused.

- **Parameters:**
  - `paused` (boolean): `true` to pause, `false` to resume

```lua
player:setPaused(true)   -- pause
player:setPaused(false)  -- resume
```

---

#### `player:setPlayRange(start, end)`
Sets the playback range in PCM frames. Playback will only play samples within this range, and a loop goes back to `start`.

- **Parameters:**
  - `start` (number): Start frame offset. At or past `end` it is ignored.
  - `end` (number): End frame offset, not included. `0` means the end of the sample.

```lua
player:setPlayRange(0, 44100)  -- only the first second of a 44100 Hz sample
```

---

#### `player:getLength()`
Returns the length of the loaded sample in PCM frames.

- **Returns:** (number) Frame count, or `0` if no sample is set

---

#### `player:setOffset(seconds)`
Seeks to a position in seconds. `play()` starts from the beginning, so a seek before it is lost: call `setOffset` after `play()`.

- **Parameters:**
  - `seconds` (number): Playback position in seconds (fractions allowed)
- **Returns:** None

```lua
player:play()
player:setOffset(1.5)  -- on from 1.5 seconds
```

---

#### `player:getOffset()`
Returns the current playback position in seconds.

- **Returns:** (number) Position in seconds

---

#### `player:setRate(rate)` / `player:getRate()`
Sets or gets the playback rate multiplier. `1.0` is normal speed, `2.0` is double speed, `0.5` is half speed.

- **Parameters:**
  - `rate` (number): Playback rate multiplier
- **Returns:** (number) Current rate (for `getRate`)

```lua
player:setRate(1.5)  -- play at 150% speed
```

---

#### `player:setFinishCallback(fn)`
Sets a callback fired when playback finishes (all repeats completed). Each SamplePlayer can have one. It runs on your app's own thread, a little after the sound ends: within a few milliseconds while your Lua code runs, or inside `sys.sleep`, `input.update` or a flush. A long blocking call holds it back.

- **Parameters:**
  - `fn` (function or nil): Callback function (called with no arguments), or `nil` to clear it

```lua
player:setFinishCallback(function()
    picocalc.sys.log("Sample playback finished")
end)
```

---

#### `player:setLoopCallback(fn)`
Sets a callback fired each time the player loops back to the start. Each SamplePlayer can have one. It is delivered as `setFinishCallback` is.

- **Parameters:**
  - `fn` (function or nil): Callback function (called with no arguments), or `nil` to clear it

```lua
player:setLoopCallback(function()
    picocalc.sys.log("Sample looped")
end)
```

---

### FilePlayer

Streams a WAV or QOA file from the SD card without loading it fully into memory.

#### `picocalc.sound.fileplayer([bufferSize])`
Creates a FilePlayer. At most 2 exist at once: a 3rd returns `nil, errstr` until one is collected. You rarely need a second, since `load` swaps the file and only one plays at a time.

- **Parameters:**
  - `bufferSize` (number, optional): Accepted and ignored. The stream's buffer has a fixed size.
- **Returns:** (userdata) FilePlayer object, or `nil, errstr` on failure

---

#### `player:load(path)`
Opens a WAV or QOA file for streaming (sniffed from the header, not the extension).

WAV streams 16-bit PCM only: an 8-bit WAV is refused here (a Sample accepts it).
QOA ("Quite OK Audio") is a lossy format a fifth the size of 16-bit PCM, and
the cheapest music format for a real-time app (see **Performance** below):
the second core decodes it a little at a time and reads a fifth of the bytes
from the SD card. Encode offline with the reference `qoaconv` tool
(https://github.com/phoboslab/qoa). Mono and stereo only, up to 192 kHz. A
file cut short (an interrupted copy) plays the whole frames it holds.

- **Parameters:**
  - `path` (string): Absolute path to a WAV or QOA file
- **Returns:** `true` if the file will play, or `nil, errstr` (`"access denied"` outside the sandbox; otherwise the reason: the file cannot be opened, it is an MP3 (use the MP3Player), an unknown format, a WAV that is not 16-bit PCM, or a QOA the player refuses). A failed `load` leaves the player empty.

---

#### `player:play([repeat])` / `player:stop()` / `player:pause()` / `player:resume()` / `player:isPlaying()`
Standard playback controls. `play()` returns `true`, or `nil, errstr` with no file loaded. `repeat` works the same as SamplePlayer: `n` plays the file `n` times and `0` loops until stopped (see Repeat counts). A player with a loop range (`setLoopRange`) loops until stopped whatever `repeat` says. `pause()` halts playback at once, keeping the position and the audio already buffered; `resume()` continues from the same sample, or from the new position after a `setOffset()` while paused (the seek drops the buffered audio).

`play()` fills the stream's buffer before the sound starts: about 10-60 ms after the call, depending on the format and the SD card (QOA is quickest, 44.1 kHz stereo WAV slowest) (the buffer holds 93 ms of 44.1 kHz audio, 186 ms of 22.05 kHz). Starting full means the SD card can be busy right after `play()` (your app loading a sample or a level, say) without a gap in the music; it just starts once the buffer is full.

---

#### `player:getLength()` / `player:getSampleRate()` / `player:getOffset()` / `player:setOffset(seconds)`
`getLength()` returns the file's length in sample frames (per channel), not seconds: divide by `getSampleRate()` for seconds. `getSampleRate()` returns the file's sample rate in Hz (`0` before a successful `load`). `getOffset()` returns the playback position and `setOffset()` seeks to one, both in whole seconds. `play()` starts from the beginning, so a seek before it is lost: call `setOffset()` after `play()`. While the player plays, a seek is heard after the audio already buffered (up to 93 ms at 44.1 kHz, 186 ms at 22.05 kHz); paused, or right after `play()` before the sound starts, the buffered audio is dropped and the new position plays first.

---

#### `player:setVolume(left [, right])` / `player:getVolume()`
Sets the volume (0–100, clamped). `right` is accepted for compatibility but ignored: both channels use `left`. `getVolume()` returns the volume twice.

---

#### `player:setLoopRange([start [, end]])`
Sets the loop region in whole seconds and makes the player loop until stopped. `play()` starts at the beginning of the file and runs to `end`, then continues from `start` at every pass (a `setOffset()` past `end` plays on to the end of the file first, then wraps to `start`). Omit `end` (or pass `0`) to loop to the end of the file, and omit both to loop the whole file. An `end` past the data is the end of the data, and an empty range (`end <= start`) loops the whole file. The loop callback fires at each wrap.

Granularity: WAV loops on the exact sample. QOA loops on the exact sample too, but the decoder can only start at a QOA frame (5120 samples): at each wrap it decodes and drops the samples between the frame start and `start`, up to 5119 of them. A QOA frame boundary is a multiple of 5120 samples, which is a whole number of seconds only at long intervals (8 s at 48 kHz, 256 s at 44.1 kHz), so in practice only `start` = 0 avoids the dropped samples.

The range stays until you `load()` another file, which clears it, so a player that has had a range loops until stopped and ignores the `repeat` count of `play(n)`. Call `setLoopRange()` after `load()`.

---

#### `player:didUnderrun()`
Returns whether the audio stream ran dry while this player was playing, since the last call (or since `play()`). An underrun means the player could not supply audio data fast enough (SD card busy, or the second core starved), and it is audible as a gap. The flag is sticky until you read it: a call returns `true` once for any number of underruns and then clears. The wait after `play()` (or `resume()`) while the player fills its buffer does not count, nor does the end of a file that finished.

- **Returns:** (boolean) `true` if an underrun occurred

```lua
if player:didUnderrun() then
    picocalc.sys.log("Audio buffer underrun!")
end
```

---

#### `player:setFinishCallback(fn)`
Sets a callback function to be called when playback finishes. Each FilePlayer can have one, delivered as a SamplePlayer's is.

- **Parameters:**
  - `fn` (function or nil): Callback function (called with no arguments), or `nil` to clear it

```lua
player:setFinishCallback(function()
    picocalc.sys.log("Playback finished")
end)
```

---

#### `player:setLoopCallback(fn)`
Sets a callback fired each time the file loops back to the start. Each FilePlayer can have one.

- **Parameters:**
  - `fn` (function or nil): Callback function (called with no arguments), or `nil` to clear it

```lua
player:setLoopCallback(function()
    picocalc.sys.log("File looped")
end)
```

---

#### `player:setRate(rate)` / `player:getRate()`
Sets or gets the playback rate. Rate is clamped to 0.1–4.0. Uses nearest-neighbor resampling.

- **Parameters:**
  - `rate` (number): Playback rate multiplier (`1.0` = normal, `2.0` = double speed, `0.5` = half speed)
- **Returns:** (number) Current rate (for `getRate`)

```lua
player:setRate(2.0)                -- double speed
local r = player:getRate()         -- returns 2.0
```

---

#### `player:setStopOnUnderrun(flag)`
Controls whether the player automatically stops when the stream underruns (see `didUnderrun()`). The player is then stopped, and `didUnderrun()` still returns `true` until you read it. The default is `false`: playback carries on after the gap.

- **Parameters:**
  - `flag` (boolean): `true` to stop on underrun, `false` to continue

```lua
player:setStopOnUnderrun(true)
```

---

### MP3Player

Streams an MP3 file from the SD card.

The MP3 plays through the same mixer as samples, the stream and tones, so music and sound effects play together; `picocalc.audio.setVolume` scales it too. Sources add up: a full-volume MP3 plus loud samples clips, so leave headroom (music at about 60).

**Performance.** MP3 decoding runs on the second core, but it shares the flash and PSRAM cache with your app, so whatever of it overlaps your frames slows your app's own code. Measured in a gfx3d game at 200 MHz paced at 30 fps: with 44.1 kHz stereo music (128 kbps) the frame work was the same as with no music (18.7 ms), and so it was with 22.05 kHz mono, because the decoding happened while each frame waited for its deadline (see **Paced apps**). In a racing game at 300 MHz (six ships, the engine note and six sound effects), 44.1 kHz stereo MP3 music at ~230 kbps cost the same frame time as the same music as QOA. An app that never paces gives the decoder nowhere else to go: there 44.1 kHz stereo cost about 21% of the frame work on average (18.1 to 21.8 ms) and 48% at the 95th percentile (18.6 to 27.6 ms), 22.05 kHz mono less. A looping WAV streamed with a [FilePlayer](#fileplayer) (`play(0)`) cost 2.5% at 22.05 kHz mono and 15% at 44.1 kHz stereo, because it reads the SD card over its own bus. A tracker module on the [MOD player](API-Modplayer.md) cost 14% with 4 channels, 24% with 8. Measured in the racing game at 200 MHz, a looping [FilePlayer](#fileplayer) track of real music cost QOA 2.4% at 22.05 kHz mono and 5.3% at 44.1 kHz stereo, against 3.9% and 14% for the same music as WAV; at 300 MHz, QOA 2.1% and 4.6%, WAV 2.7% and 11%. For music in a real-time game: MP3 if the game paces itself with `perf.setTargetFPS` and its frames leave some time to spare; otherwise QOA, then WAV, or a MOD if you can spare the frame time.

**Paced apps.** When your app paces itself with [`perf.setTargetFPS`](API-Performance.md), the MP3Player decodes ahead while `perf.endFrame()` waits for the frame's deadline, half a frame of MP3 (about 2.3 ms of work for 13 ms of 44.1 kHz stereo audio at 200 MHz) at a time, so its decoding stays out of your frames. It needs that wait: a wait shorter than one such piece (about 2.5 ms at 44.1 kHz stereo, 1.5 ms at 22.05 kHz mono, at 200 MHz) goes unused, and at 30 fps 44.1 kHz stereo needs two or three pieces a frame, so about 7 ms of wait a frame to be decoded entirely there (22.05 kHz mono: about 3 ms). Frames that use more of their period (or an app that never paces) leave the rest nowhere else to go, and it decodes during your frames instead.

#### `picocalc.sound.mp3player()`
Creates an MP3Player.

`picocalc.sound.mp3player()` returns the one live MP3 player handle (there is a single MP3 decoder).

- **Returns:** (userdata) MP3Player object, or `nil, errstr` on failure

```lua
local mp3 = picocalc.sound.mp3player()
mp3:load("/apps/myapp/music.mp3")
mp3:play()
```

---

#### `player:load(path)`
Opens an MP3 file for streaming. ID3v2 tags at the start of the file (title, artist, cover art; any size) are skipped: playback starts, and every loop restarts, at the first frame of audio. ID3 tags further on (an ID3v1 or ID3v2 tag after the audio, or between two MP3s joined into one file) are skipped when the decoder reaches them, and cost nothing however large. A tag is skipped only if its size fits the file and the stream goes on where it ends; otherwise its bytes are searched for the next frame, which holds the decoder up by ~1 ms for every 32 bytes (no audio is lost).

- **Returns:** `true` on success, or `nil, errstr` if the file cannot be opened or is not an MP3

---

#### `player:play([repeat])` / `player:stop()` / `player:pause()` / `player:resume()` / `player:isPlaying()`
Standard playback controls. `play()` returns `true`, or `nil, errstr` with no file loaded. `play(repeat)` here only chooses between looping and not: `0` loops until stopped, any other value plays once (the count is not honoured; `setLoop()` sets the same flag). `play()` starts from the beginning of the file (also after it finished). `stop()`, `pause()` and a `play()` while playing fade out over ~1.5 ms (no click); `resume()` fades back in. `isPlaying()` stays `true` until the last decoded audio has played.

---

#### `player:getPosition()` / `player:getLength()`
`getPosition()` returns the frames played since `play()` (divide by `getSampleRate()` for seconds; it keeps counting across loops, and holds while paused); it returns `0` once the MP3 is stopped or has finished. `getLength()` returns `0`: an MP3's length is not known without decoding all of it.

---

#### `player:getSampleRate()`
Returns the sample rate of the MP3 file in Hz.

- **Returns:** (number)

---

#### `player:setVolume(vol)` / `player:getVolume()`
Volume range 0–100 (larger values clamp to 100).

---

#### `player:setLoop(loop)`
- **Parameters:**
  - `loop` (boolean): `true` to loop continuously
