---
title: "API Audio and Sound"
---

Audio output, including simple tones and full sample/file playback.

## picocalc.audio

Tones, the master volume and a raw PCM stream. One mixer sums everything that makes sound (tones, the stream, SamplePlayers, the FilePlayer, the MOD player and the MP3Player) into a single 44.1 kHz output, so they all play at once.

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
Sets the master volume. It scales everything the mixer plays.

- **Parameters:**
  - `volume` (number): Volume level (0–100, where 0 is muted and 100 is maximum; larger values clamp to 100)
- **Returns:** None

```lua
picocalc.audio.setVolume(50)  -- half volume
```

---

### PCM Streaming

#### `picocalc.audio.startStream(sampleRate)`
Starts (or restarts) the PCM stream at `sampleRate` and empties its buffer. Other sounds keep playing. The FilePlayer and the MOD player use this stream while they play.

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
Get the number of free slots available in the audio ring buffer. Use this to avoid pushing more samples than the buffer can hold. While no stream is started it reports the whole (empty) buffer.

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
- **FilePlayer** — streams a WAV file from the SD card
- **MP3Player** — streams an MP3 file from the SD card

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
Sets a callback fired when playback finishes (all repeats completed). Maximum 4 callbacks across all SamplePlayer instances. The callback fires on Core 0 via the Lua instruction hook (slight delay of up to ~256 opcodes).

- **Parameters:**
  - `fn` (function): Callback function (called with no arguments)

```lua
player:setFinishCallback(function()
    picocalc.sys.log("Sample playback finished")
end)
```

---

#### `player:setLoopCallback(fn)`
Sets a callback fired each time the player loops back to the start. Same cross-core delivery mechanism as `setFinishCallback`.

- **Parameters:**
  - `fn` (function): Callback function (called with no arguments)

```lua
player:setLoopCallback(function()
    picocalc.sys.log("Sample looped")
end)
```

---

### FilePlayer

Streams a WAV file from the SD card without loading it fully into memory.

#### `picocalc.sound.fileplayer([bufferSize])`
Creates a FilePlayer.

- **Parameters:**
  - `bufferSize` (number, optional): Internal streaming buffer size in bytes
- **Returns:** (userdata) FilePlayer object, or `nil, errstr` on failure

---

#### `player:load(path)`
Opens a WAV file for streaming.

Streams 16-bit PCM only: an 8-bit WAV is refused here (a Sample accepts it).

- **Parameters:**
  - `path` (string): Absolute path to a WAV file
- **Returns:** `true` on success, or `nil, errstr`

---

#### `player:play([repeat])` / `player:stop()` / `player:pause()` / `player:resume()` / `player:isPlaying()`
Standard playback controls. `repeat` works the same as SamplePlayer. `pause()` halts playback keeping the position; `resume()` continues from the paused position.

---

#### `player:getLength()` / `player:getOffset()` / `player:setOffset(seconds)`
Returns or seeks to a position in seconds.

---

#### `player:setVolume(left [, right])` / `player:getVolume()`
Sets the volume (0–100, clamped). `right` is accepted for compatibility but ignored: both channels use `left`. `getVolume()` returns the volume twice.

---

#### `player:setLoopRange([start [, end]])`
Sets the loop region in seconds. Omit both to loop the whole file.

---

#### `player:didUnderrun()`
Returns whether the streaming buffer underran since the last check. An underrun means the SD card could not supply audio data fast enough.

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
Controls whether the player automatically stops when a buffer underrun occurs.

- **Parameters:**
  - `flag` (boolean): `true` to stop on underrun, `false` to continue

```lua
player:setStopOnUnderrun(true)
```

---

### MP3Player

Streams an MP3 file from the SD card.

The MP3 plays through the same mixer as samples, the stream and tones, so music and sound effects play together; `picocalc.audio.setVolume` scales it too. Sources add up: a full-volume MP3 plus loud samples clips, so leave headroom (music at about 60).

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
Standard playback controls. `play()` starts from the beginning of the file (also after it finished). `stop()`, `pause()` and a `play()` while playing fade out over ~1.5 ms (no click); `resume()` fades back in. `isPlaying()` stays `true` until the last decoded audio has played.

---

#### `player:getPosition()` / `player:getLength()`
`getPosition()` returns the frames played since `play()` (divide by `getSampleRate()` for seconds; it keeps counting across loops). `getLength()` returns `0`: an MP3's length is not known without decoding all of it.

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
