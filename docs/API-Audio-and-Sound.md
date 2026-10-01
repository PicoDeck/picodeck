---
title: "API Audio and Sound"
---

Audio output, including simple tones and full sample/file playback.

## picocalc.audio

Tones, the master volume and a raw PCM stream. One mixer sums everything that makes sound into a single 44.1 kHz output. The FilePlayer, the MOD player and `startStream` share one PCM stream, so only one of them may play at a time. Stop the one that is playing before starting another: the only automatic stop is that starting a FilePlayer stops another FilePlayer. Tones, SamplePlayers and the MP3Player mix with the stream and with each other, so they all play at once.

### Functions

#### `picocalc.audio.playTone(freq [, duration])`
Plays a tone at the specified frequency.

- **Parameters:**
  - `freq` (number): Frequency in Hz (e.g., `440` for concert A)
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

- **Parameters:**
  - `volume` (number): Volume level (0–100, where 0 is muted and 100 is maximum; larger values clamp to 100)
- **Returns:** None

```lua
picocalc.audio.setVolume(50)  -- half volume
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
Push audio samples to the streaming buffer. Samples are interleaved stereo pairs (left, right, left, right...). Only while a stream is started (`startStream`): pushes before it, or after `stopStream`, are dropped.

- **Parameters:**
  - `samples` (table): Array of int16 sample values (max 512 values = 256 stereo pairs)
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
Get the number of free slots available in the audio ring buffer. Use this to avoid pushing more samples than the buffer can hold. While no stream is started it reports 0 (pushes are dropped then).

- **Parameters:** None
- **Returns:** (number) Free buffer slots

```lua
local free = picocalc.audio.ringFree()
if free >= 512 then
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
Returns the current audio clock time in milliseconds since the last `resetTime()` call.

- **Returns:** (number) Milliseconds

---

#### `picocalc.sound.resetTime()`
Resets the audio clock to zero.

- **Returns:** None

---

#### `picocalc.sound.playingSources()`
Returns the number of audio sources currently playing across all player types.

- **Returns:** (number) Count of active audio sources

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
  - `path` (string, optional): Absolute path to a WAV file
- **Returns:** (userdata) Sample object, or `nil, errstr` on failure

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
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
  - `end` (number): End offset in PCM frames
- **Returns:** (userdata) New Sample object, or `nil, errstr` on failure

```lua
local clip = sample:getSubsample(0, 22050)  -- first second at 44100 Hz
```

---

#### `sample:play([repeatCount [, rate]])`
Creates a temporary SamplePlayer, starts playback, and returns the player. Convenience method.

- **Parameters:**
  - `repeatCount` (number, optional): Number of times to play (default `1`)
  - `rate` (number, optional): Playback rate multiplier (default `1.0`)
- **Returns:** (userdata) SamplePlayer object

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
s:play()           -- play once at normal speed
s:play(3, 1.5)     -- play 3 times at 150% speed
```

---

#### `sample:playAt(when [, vol [, rightvol [, rate]]])`
Creates a temporary SamplePlayer and starts playback. The `when` parameter is accepted for API compatibility but ignored on this hardware (playback starts immediately).

- **Parameters:**
  - `when` (number): Ignored (accepted for API compatibility)
  - `vol` (number, optional): Volume 0–100 (default `100`; larger values clamp)
  - `rightvol` (number, optional): Ignored (mono PWM output)
  - `rate` (number, optional): Playback rate multiplier (default `1.0`)
- **Returns:** (userdata) SamplePlayer object

```lua
local s = picocalc.sound.sample("/apps/myapp/beep.wav")
local player = s:playAt(0, 200)       -- play at volume 200
local player = s:playAt(0, 128, 0, 2.0)  -- play at double speed
```

---

#### `sample:save(filename)`
Writes the sample data to a WAV file on the SD card.

- **Parameters:**
  - `filename` (string): Path to write
- **Returns:** `true` on success, or `false, errstr` on failure

```lua
local clip = sample:getSubsample(0, 22050)
clip:save("/data/com.myapp/clip.wav")
```

---

### SamplePlayer

Plays a `Sample` from memory. Supports looping and volume control.

#### `picocalc.sound.sampleplayer([sample_or_path])`
Creates a SamplePlayer, optionally pre-loading a sample.

A SamplePlayer keeps its Sample alive (you may drop your own reference). `sampleplayer(path)` and `sample:play()` create a Sample of their own. Dropping the player returned by `sample:play()` stops that sound when it is collected.

At most 8 SamplePlayers exist at once (the mixer's voices): a 9th returns `nil, errstr` until one is collected. Reuse players and call `setSample`, or keep a pool.

- **Parameters:**
  - `sample_or_path` (userdata or string, optional): A `Sample` object or a WAV file path
- **Returns:** (userdata) SamplePlayer object, or `nil, errstr` on failure

```lua
local player = picocalc.sound.sampleplayer("/apps/myapp/beep.wav")
player:play()
```

---

#### `player:setSample(sample)`
Sets the sample to play.

- **Parameters:**
  - `sample` (userdata): A `Sample` object
- **Returns:** `true` on success, or `nil, errstr`

---

#### `player:play([repeat])`
Starts playback.

- **Parameters:**
  - `repeat` (number, optional): Number of times to repeat. `0` loops indefinitely.
- **Returns:** `true` if started

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
Pauses or unpauses playback without resetting the playback position.

- **Parameters:**
  - `paused` (boolean): `true` to pause, `false` to resume

```lua
player:setPaused(true)   -- pause
player:setPaused(false)  -- resume
```

---

#### `player:setPlayRange(start, end)`
Sets the playback range in PCM frames. Playback will only play samples within this range.

- **Parameters:**
  - `start` (number): Start frame offset
  - `end` (number): End frame offset

```lua
player:setPlayRange(0, 44100)  -- play only the first second
```

---

#### `player:getLength()`
Returns the length of the loaded sample in PCM frames.

- **Returns:** (number) Frame count, or `0` if no sample is set

---

#### `player:setOffset(seconds)`
Seeks to a position in seconds.

- **Parameters:**
  - `seconds` (number): Playback position in seconds (fractions allowed)
- **Returns:** None

```lua
player:setOffset(1.5)  -- seek to 1.5 seconds
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
Sets a callback fired when playback finishes (all repeats completed). Each SamplePlayer can have one. The callback fires on Core 0 via the Lua instruction hook (slight delay of up to ~256 opcodes).

- **Parameters:**
  - `fn` (function): Callback function (called with no arguments)

```lua
player:setFinishCallback(function()
    picocalc.sys.log("Sample playback finished")
end)
```

---

#### `player:setLoopCallback(fn)`
Sets a callback fired each time the player loops back to the start. Each SamplePlayer can have one. Same cross-core delivery mechanism as `setFinishCallback`.

- **Parameters:**
  - `fn` (function): Callback function (called with no arguments)

```lua
player:setLoopCallback(function()
    picocalc.sys.log("Sample looped")
end)
```

---

### FilePlayer

Streams a WAV or QOA file from the SD card without loading it fully into memory.

#### `picocalc.sound.fileplayer([bufferSize])`
Creates a FilePlayer.

- **Parameters:**
  - `bufferSize` (number, optional): Internal streaming buffer size in bytes
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
Standard playback controls. `repeat` works the same as SamplePlayer: `n` plays the file `n` times and `0` loops until stopped (see Repeat counts). A player with a loop range (`setLoopRange`) loops until stopped whatever `repeat` says. `pause()` halts playback at once, keeping the position and the audio already buffered; `resume()` continues from the same sample, or from the new position after a `setOffset()` while paused (the seek drops the buffered audio).

`play()` fills the stream's buffer before the sound starts: about 10-60 ms after the call, depending on the format and the SD card (QOA is quickest, 44.1 kHz stereo WAV slowest) (the buffer holds 93 ms of 44.1 kHz audio, 186 ms of 22.05 kHz). Starting full means the SD card can be busy right after `play()` (your app loading a sample or a level, say) without a gap in the music; it just starts once the buffer is full.

---

#### `player:getLength()` / `player:getSampleRate()` / `player:getOffset()` / `player:setOffset(seconds)`
`getLength()` returns the file's length in sample frames (per channel), not seconds: divide by `getSampleRate()` for seconds. `getSampleRate()` returns the file's sample rate in Hz (`0` before a successful `load`). `getOffset()` returns the playback position and `setOffset()` seeks to one, both in whole seconds. While the player plays, a seek is heard after the audio already buffered (up to 93 ms at 44.1 kHz, 186 ms at 22.05 kHz); paused, or right after `play()` before the sound starts, the buffered audio is dropped and the new position plays first.

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
Sets a callback function to be called when playback finishes. Maximum 2 finish callbacks across all FilePlayer instances.

- **Parameters:**
  - `fn` (function): Callback function (called with no arguments)

```lua
player:setFinishCallback(function()
    picocalc.sys.log("Playback finished")
end)
```

---

#### `player:setLoopCallback(fn)`
Sets a callback fired each time the file loops back to the start. Maximum 2 loop callbacks across all FilePlayer instances.

- **Parameters:**
  - `fn` (function): Callback function (called with no arguments)

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

**Performance.** MP3 decoding runs on the second core, but it shares the flash and PSRAM cache with your app, so it slows your app's own code. Measured in a gfx3d game at 200 MHz: 44.1 kHz stereo MP3 music made every frame about 2.1× slower, 22.05 kHz mono about 1.24×. A looping WAV streamed with a [FilePlayer](#fileplayer) (`play(0)`) cost 2.5% at 22.05 kHz mono and 15% at 44.1 kHz stereo, because it reads the SD card over its own bus. A tracker module on the [MOD player](API-Modplayer.md) sits between the two: 14% with 4 channels, 24% with 8. Measured in a gfx3d racing game (six ships, 200 MHz), a looping [FilePlayer](#fileplayer) track of real music cost QOA 2.4% at 22.05 kHz mono and 5.3% at 44.1 kHz stereo, against 3.9% and 14% for the same music as WAV; at 300 MHz, QOA 2.1% and 4.6%, WAV 2.7% and 11%. For music in a real-time game, use QOA, then WAV, or a MOD if you can spare the frame time.

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
Opens an MP3 file for streaming.

- **Returns:** `true` on success, or `nil, errstr` if the file cannot be opened or is not an MP3

---

#### `player:play([repeat])` / `player:stop()` / `player:pause()` / `player:resume()` / `player:isPlaying()`
Standard playback controls. `play(repeat)` here only chooses between looping and not: `0` loops until stopped, any other value plays once (the count is not honoured; `setLoop()` sets the same flag). `play()` starts from the beginning of the file (also after it finished). `stop()`, `pause()` and a `play()` while playing fade out over ~1.5 ms (no click); `resume()` fades back in. `isPlaying()` stays `true` until the last decoded audio has played.

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
