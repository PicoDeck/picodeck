---
title: "API — Video Playback"
---

PicoDeck supports MJPEG video playback from AVI files stored on the SD card. Video is decoded on Core 0 using JPEGDEC and displayed via the standard display framebuffer. Audio tracks (MP3) are decoded on Core 1 via a shared ring buffer.

**Not in the simulator.** `picocalc.video` has no decoder there: `player()` returns `nil, "failed to create video player"`. Test video on a device.

Video needs no entry in your app's `requirements`, with or without an audio track.

---

## Lua API

### Creating a player

```lua
local player = picocalc.video.player()
```

#### `picocalc.video.player()`
Creates a new video player instance.

- **Returns:** (userdata) Video player object, or `nil, errstr` on failure

### Loading and playing

```lua
local ok, err = player:load("/apps/myvideo/clip.avi")  -- path on SD card
player:play()
player:stop()
-- no destroy(): the player is freed automatically by Lua garbage collection
```

`load` returns `true`, or `false` if the file cannot be used. Only a path outside the sandbox gives a reason: `false, "access denied"`.

### Playback control

```lua
player:setLoop(true)         -- loop continuously (default: false — hold the last frame)
player:seek(frame_number)    -- jump to a specific frame index
player:seekMs(ms)            -- jump to an absolute time
player:seekRelativeMs(-5000) -- skip backwards/forwards relative to the current position
```

All seeks clamp to `[0, last frame]` and **never wrap**: seeking to or past the last frame ends the video on the next `update()` (held, or looped if `setLoop(true)`). A seek while paused decodes and presents the target frame immediately, so scrubbing gives feedback, and the audio starts from the target on `resume()`. A seek on an ended player restarts playback from the target. A seek while playing switches the audio to the new position with a quick fade (under a millisecond of silence between the two): the old audio plays on while the new position's audio is read and its first frames are decoded. `seek()` takes as long as that read and decode (up to ~120 ms on the device).

A looping video's audio plays on through the loop point without restarting, and the video restarts when the audio's loop point plays. What remains there is the MP3's own encoder delay and padding, ~25-50 ms of near-silence at each loop. When the audio track is a little longer or shorter than the video (an MP3 usually runs a frame or two longer), the video holds its last frame, or skips its first frames, for the difference: up to 200 ms. Tracks further apart than that both restart at every loop, the audio with the quick fade of a seek (and its encoder delay).

#### `player:pause()`
Pauses video playback. Audio is also paused. The current frame is presented as a stable still (with the OSD) so the app can draw overlays and call `display.flush()` freely.

#### `player:resume()`
Resumes video playback after a pause. On an ended player this replays from the start (same as `play()`).

#### End of video
With looping off, reaching the last frame holds it on screen: `isPlaying()` becomes `false`, `hasEnded()` becomes `true`, and the OSD shows the bar at 100% with `END`. Call `play()` (or `resume()`) to replay, or seek to continue from elsewhere.

---

### Position and duration

```lua
player:getFrameCount()   -- total frames
player:getDurationMs()   -- total length in milliseconds
player:getPositionMs()   -- position of the frame on screen, in milliseconds
player:hasEnded()        -- true once the last frame was reached (loop off)
```

---

### Progress OSD

The player draws its own on-screen display over the bottom of the video area: a progress bar with a playhead, elapsed time on the left, total time on the right, and a pause glyph or `END` marker in the middle. It is enabled by default.

```lua
player:setOSD(true)          -- enable/disable (default: enabled)
player:showOSD()             -- show now and restart the hide timer
player:setOSDTimeout(3000)   -- ms to stay visible while playing (default 3000)
```

The OSD appears automatically on `play()`, `pause()`, `resume()` and every seek. While playing it hides after the timeout; while paused or ended it stays. It is drawn into the decoded frame before the frame is flushed, so it costs nothing extra to present and works with both `setAutoFlush(true)` and manual flushing. Videos shorter than 48 px tall skip the OSD.

---

### Performance and stats

#### `player:getFPS()`
Returns the actual playback frame rate.

- **Returns:** (number) Frames per second

```lua
local fps = player:getFPS()
picocalc.sys.log("FPS: " .. string.format("%.1f", fps))
```

#### `player:getSize()`
Returns the video frame dimensions.

- **Returns:** (number, number) Width and height in pixels

```lua
local w, h = player:getSize()
```

#### `player:getDroppedFrames()`
Returns the number of frames dropped during playback due to decode not keeping up with the frame rate.

- **Returns:** (number) Number of dropped frames

#### `player:resetStats()`
Resets playback statistics (dropped frame counter).

#### `player:setAutoFlush(enabled)`
Controls whether the player presents each decoded frame itself. When disabled, the app must call `display.flush()` once for each decoded frame, allowing HUD overlays to be drawn between decode and flush: see [Game loop integration](#game-loop-integration).

- **Parameters:**
  - `enabled` (boolean): `true` for automatic flush (default), `false` for manual flush

```lua
player:setAutoFlush(false)  -- manual flush for HUD overlay
```

---

### Audio control

```lua
player:hasAudio()            -- true if the AVI file contains an MP3 audio track
player:setMuted(true)        -- mute audio (video still plays)
player:isMuted()             -- returns current mute state
player:setVolume(vol)        -- 0–100
player:getVolume()           -- returns current volume
```

The volume belongs to the player: set it before or after `play()`, and it holds across loops, seeks, mute and `load()` (default 100). Unmuting while paused starts the audio on `resume()`.

### State

```lua
player:isPlaying()           -- true while playing
player:isPaused()            -- true while paused
local info = player:getInfo()
-- info.width, info.height   — frame dimensions
-- info.frames               — total frames in file
-- info.current_frame        — current playback position
-- info.dropped_frames       — frames dropped during playback
-- info.has_audio            — whether an audio track was found
-- info.duration_ms          — total length in milliseconds
-- info.position_ms          — position of the frame on screen
-- info.ended                — true once the last frame was reached (loop off)
-- info.fps                  — content frame rate
```

### Game loop integration

Call `player:update()` on every pass of your loop, as often as you can: it decodes a frame when one is due and does nothing otherwise.

`update()` returns `true` only on a pass that decoded a frame. It returns `false` between frames, as well as when paused, ended or not playing, so a loop that runs `while player:update()` stops at the first early pass. Use `isPlaying()` or `hasEnded()` to see whether the video is still going.

With auto-flush on (the default) the player presents each frame itself, at the start of the next `update()`. Do not call `display.flush()` in this loop: a flush swaps the two framebuffers, so the panel would alternate between the frame and the older one in the other buffer, and the picture flickers.

```lua
local player = picocalc.video.player()
player:load("/apps/demo/video.avi")
player:play()

while true do
    player:update()

    picocalc.input.update()
    local pressed = picocalc.input.getButtonsPressed()
    if pressed & picocalc.input.BTN_ESC ~= 0 then break end
end

player:stop()
```

To draw over the video, turn auto-flush off and present each frame yourself, only on a pass that decoded one. Draw the overlay every time: each frame repaints the rows it covers.

```lua
local player = picocalc.video.player()
player:load("/apps/demo/video.avi")
player:setAutoFlush(false)
player:play()

while true do
    if player:update() then
        picocalc.display.drawText(4, 4, "HUD", picocalc.display.WHITE, false)
        picocalc.display.flush()
    end

    picocalc.input.update()
    local pressed = picocalc.input.getButtonsPressed()
    if pressed & picocalc.input.BTN_ESC ~= 0 then break end
end

player:stop()
```

---

## Video format

PicoDeck plays **MJPEG AVI** files only. Each video frame is an independent JPEG; the player does not support inter-frame codecs (H.264, VP9, etc.).

| Property | Value |
|----------|-------|
| Container | AVI (RIFF) |
| Video codec | MJPEG (Motion JPEG) |
| Audio codec | MP3 (MPEG-1 Audio Layer III) |
| Resolution | Up to 320 × 320 (native display size) |
| Frame rate | 25 fps recommended |
| Audio sample rate | 22050 Hz or 44100 Hz |
| Audio channels | Mono or stereo |

---

## Converting video with `video_converter.sh`

The `tools/video_converter.sh` script converts any video file (or YouTube URL) to the correct AVI format using `ffmpeg`.

### Prerequisites

```bash
# Install ffmpeg (required)
sudo apt install ffmpeg          # Debian/Ubuntu
brew install ffmpeg              # macOS
sudo dnf install ffmpeg          # Fedora

# Install yt-dlp (only needed for URL downloads)
pip install yt-dlp
```

### Usage

```bash
tools/video_converter.sh [options] <file_or_url>
```

Output is written to `~/Videos/PicoDeck/` (the directory must exist), named after the input file, or after the video's title for a URL. Options come before the file or URL.

### Options

| Flag | Description | Default |
|------|-------------|---------|
| `--crop` | Scale to 320×320, cropping to fill (square crop) | off (letterbox) |
| `--quality N` | MJPEG quality, 1 (best) – 31 (worst) | `8` |
| `--mute` | Strip the audio track entirely | off (include audio) |
| `--chroma444` | Keep full-resolution colour (`yuvj444p`); roughly doubles decode time | off (`yuvj420p`) |

### Examples

```bash
# Basic conversion — letterbox to 320×height, include audio
tools/video_converter.sh myclip.mp4

# Square crop for portrait/square content
tools/video_converter.sh --crop myclip.mp4

# High quality, muted (video-only)
tools/video_converter.sh --quality 3 --mute myclip.mp4

# Download and convert directly from YouTube
tools/video_converter.sh "https://www.youtube.com/watch?v=..."

# Download, crop, strip audio
tools/video_converter.sh --crop --mute "https://youtu.be/..."
```

### What the converter does

**Without `--crop`** (letterbox):
```
ffmpeg -i <input>
  -vf "fps=25,scale=320:-1"
  -vcodec mjpeg -pix_fmt yuvj420p -q:v <quality> -huffman optimal
  -acodec libmp3lame -ab 128k -ar 22050
  <output>.avi
```

**With `--crop`** (square fill):
```
ffmpeg -i <input>
  -vf "fps=25,scale=320:320:force_original_aspect_ratio=increase,crop=320:320"
  -vcodec mjpeg -pix_fmt yuvj420p -q:v <quality> -huffman optimal
  -acodec libmp3lame -ab 128k -ar 22050
  <output>.avi
```

**With `--mute`**: replaces the `-acodec libmp3lame ...` line with `-an` (no audio).

**Chroma subsampling**: 4:2:0 (`yuvj420p`) is forced because it halves the decoder's chroma work on the RP2350 and is visually indistinguishable on the 320×320 panel for real footage. ffmpeg only picks it by itself when the source is already 4:2:0, so 4:4:4 or RGB sources would otherwise decode about twice as slowly. `--chroma444` keeps full-resolution colour for the rare clip that needs it.

---

## How the audio plays

A video's audio plays through the one MP3 decoder: the player reads the AVI's MP3 chunks from the card and feeds them to it as the video runs. The mixer mixes it with samples and tones, as it does any MP3. So a video with an audio track takes the place of an [MP3Player](API-Audio-and-Sound.md#mp3player): it closes a file the MP3Player has loaded, and you `load` that again after the video.

Audio playback is automatically stopped when `player:stop()` is called or the player is garbage-collected.

---

## Clock speed during playback

When `player:play()` is called, the system clock is raised to **300 MHz** to fit MJPEG decode within the 40 ms frame budget. It stays there while paused and after the end of the video, until `player:stop()` or until the player is garbage collected. Then it goes back to what it was: 200 MHz, or the clock your app asked for with `system_clock_khz` in `app.json`. With two players, the clock goes back when the last of them stops.

**WiFi is disconnected** for the same stretch, muted or not, because the radio's traffic and power are not wanted during playback, and connections open at that moment fail. No API plays a video with WiFi left up. If WiFi was connected before `play()`, it reconnects after the clock goes back, to the network saved in the system settings, even if your app had joined another. The reconnect runs in the background, so check [`picocalc.network.getStatus()`](API-Network-and-WiFi.md) before your next request.

---

## See also

- [Crash Logging and Watchdog](Crash-Logging-and-Watchdog.md) — fault recovery and reboot behavior
- [API — Audio and Sound](API-Audio-and-Sound.md) — MP3 player, sound samples, tone generation
- [API — Display and Graphics](API-Display-and-Graphics.md) — display primitives used alongside video
