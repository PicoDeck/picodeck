---
title: "API 3D"
---

## picocalc.gfx3d

A small retained-mode 3D renderer for low-poly scenes: flat-shaded and textured triangle meshes, a camera, one directional light, distance fog, a sky of horizon-aligned colour bands or an image, background scenery, camera-facing sprites and a mirror plane for reflections.

Your Lua code builds meshes once and then, every frame, sets the camera and calls `draw` for each object between `beginScene()` and `endScene()`. Transform, culling, lighting, fog, clipping and depth ordering all run in C. `draw` does its work at once and keeps no reference to the mesh; `drawTextured` keeps its mesh until the next `beginScene()` ([Textured meshes](#textured-meshes)).

Objects are drawn far to near (painter's order), with no depth buffer. Keep triangles reasonably short, so that long faces don't sort in front of objects above them. Use `bias` or `sortAsOne` for objects that sit on other geometry.

Native apps have the same renderer through [`api->gfx3d`](Native-Loading.md#3d): textured meshes, the sky image, the mirror and sprite fog as here, and whole-image sprites drawn many to a call; no flat meshes, light, sky bands, background scenery or viewport.

### Conventions

- **Coordinates are right-handed with y up.** The camera looks along **−z**, with +x to its right.
- **Angles are in radians.** A rotation is yaw (about y), then pitch (about x), then roll (about z):
  - positive yaw turns left;
  - positive pitch looks up;
  - positive roll banks left.
- **A model with no rotation faces −z**, like the camera.
- **A triangle's front is the side from which its vertices run counter-clockwise.** Back faces are skipped unless the triangle is `DOUBLE_SIDED`.
- **Colours are RGB565 integers**, from `picocalc.display.rgb(r, g, b)`.

### Defaults

Set per app, on first use:

| Setting | Default |
|---|---|
| Viewport | Full screen (320×320) |
| Vertical field of view | 60° (1.047 rad) |
| Near / far planes | 0.5 / 1000 |
| Camera | At the origin, looking along −z |
| Light | Towards (0.3, 1, 0.5), ambient 0.25 |
| Fog | Off, and for sprites off even with fog on |
| Sky | Off: no bands, no image, every row of the viewport |
| Mirror | Off |

### Functions

#### `picocalc.gfx3d.newMesh(verts, tris, colors [, flags])`
Builds a mesh.

- **Parameters:**
  - `verts` (table): Flat list of coordinates `{x1, y1, z1, x2, y2, z2, ...}`. At most 4096 vertices.
  - `tris` (table): Flat list of 1-based vertex indices, three per triangle, `{a1, b1, c1, ...}`. At most 8192 triangles.
  - `colors` (integer or table): One colour for every triangle, or one per triangle.
  - `flags` (integer or table, optional): `DOUBLE_SIDED`, `UNLIT` and `NO_FOG` added together, for all triangles or one value per triangle.
- **Returns:** a mesh. It is freed when collected.

```lua
local g3 = picocalc.gfx3d
local pyramid = g3.newMesh(
    { 0, 1, 0,  -1, 0, 1,  1, 0, 1,  1, 0, -1,  -1, 0, -1 },
    { 1, 2, 3,  1, 3, 4,  1, 4, 5,  1, 5, 2 },
    picocalc.display.rgb(220, 60, 60))
```

---

#### `mesh:getInfo()`
- **Returns:** `nverts, ntris, cx, cy, cz, radius`, the vertex and triangle counts and the bounding sphere. Culling tests this sphere, so meshes can be built directly in world coordinates.

---

#### `picocalc.gfx3d.setViewport(x, y, w, h)`
Sets the screen rectangle the scene is drawn into. The projection is centred on it. The rectangle is clipped to the screen, and it is an error if nothing is left.

---

#### `picocalc.gfx3d.setProjection(fovY, near, far)`
Sets the vertical field of view (0.01–3.0 radians) and the near and far planes (0 < near < far). Triangles cut by the near plane are clipped. Objects wholly beyond `far` are skipped.

---

#### `picocalc.gfx3d.setCamera(x, y, z, yaw, pitch, roll)`
Places the camera from a position and angles.

---

#### `picocalc.gfx3d.lookAt(ex, ey, ez, tx, ty, tz [, ux, uy, uz])`
Places the camera at `e` looking at `t`. `u` is the up direction (default `0, 1, 0`): tilting it rolls the camera. That suits a chase camera that follows a banked track.

---

#### `picocalc.gfx3d.setLight(dx, dy, dz [, ambient])`
Sets the direction **towards** the light and the ambient level (default 0.25). Each face is lit by `ambient + (1 − ambient) · max(0, n·l)`. `ambient` is clamped to 0–1; values outside that range are not an error.

---

#### `picocalc.gfx3d.setFog(near, far, color)` / `picocalc.gfx3d.setFog(nil)`
Blends each face towards `color`, linearly from depth `near` to depth `far`. `setFog(nil)` turns fog off.

---

#### `picocalc.gfx3d.setSky(bands)` / `picocalc.gfx3d.setSky(nil)`
Fills the viewport before drawing with colour bands parallel to the horizon. The bands follow the camera's pitch and roll.

- `bands` is a list of 1–8 `{angle, color}` pairs, with elevation angles in radians increasing within ±89°.
- A band covers from its angle up to the next band's angle. The first band also covers everything below it, and the last band everything above.
- Put a negative angle first to colour the ground.

```lua
local rgb = picocalc.display.rgb
g3.setSky({ { -1.2, rgb(40, 90, 50) }, { 0, rgb(250, 190, 130) },
            { 0.1, rgb(80, 120, 220) }, { 0.4, rgb(30, 40, 120) } })
```

---

#### `picocalc.gfx3d.beginScene([clearColor])`
Starts a frame. The viewport is filled at `endScene()`, with the sky if one is set, otherwise with `clearColor` if one is given. Without either, whatever is already in the back buffer stays.

---

#### `picocalc.gfx3d.draw(mesh, x, y, z, yaw, pitch, roll [, scale [, bias [, sortAsOne]]])`
Draws one instance of a mesh at a position and rotation.

- `scale` (default 1) must be positive.
- `bias` (world units, default 0) is added to the depth used for ordering. A positive bias draws the instance earlier, that is behind.
- `sortAsOne = true` orders the whole instance by the depth of its origin (plus `bias`), with its own faces sorted far to near. Use it for ships and props that sit on long road or ground triangles.

---

#### `picocalc.gfx3d.drawBasis(mesh, x, y, z, fx, fy, fz, ux, uy, uz [, scale [, bias [, sortAsOne]]])`
Like `draw`, but oriented by a forward vector (the model's −z points along it) and an up vector. Use it to align an object with a track's surface.

---

#### `picocalc.gfx3d.drawList(list [, bias [, start]])`
Draws every mesh in the array `list` as it is, in world coordinates with no transform: the same as `draw(m, 0, 0, 0, 0, 0, 0, 1, bias)` for each, in one call.

- `bias` (default 0) applies to every mesh, as in `draw`.
- `start` (default 1) is the first mesh drawn; the rest follow in order and wrap round to `list[start - 1]`, so you can submit the meshes nearest the camera first.
- An element that is not a mesh raises an error naming it (`list[3]`), as does a `start` outside 1 to `#list`. An empty list draws nothing.

Use it for scenery built in world coordinates, such as track sections: one `drawList` costs far less than a `draw` per mesh.

---

#### `picocalc.gfx3d.drawBackground(mesh)`
Draws distant scenery, such as a mountain ring, centred on the camera. It turns with the camera but never moves with it. It is never fogged or far-clipped, and it is drawn before everything else, in the order submitted.

---

#### `picocalc.gfx3d.drawSprite(image, x, y, z, size [, sx, sy, sw, sh [, bias]])`
Draws a camera-facing image centred at `(x, y, z)`, `size` world units tall, sorted with the triangles.

- `sx, sy, sw, sh` choose a source rectangle, such as a frame of a sheet.
- The image's transparent colour is honoured.
- The image stays alive until the next `beginScene()`.
- Unlike `image:drawStretched`'s `{x, y, w, h}` table, the source rectangle here is four positional numbers, so a per-frame sprite call allocates nothing.

---

#### `picocalc.gfx3d.endScene()`
Fills the viewport (sky or clear colour) and draws everything, far to near, into the back buffer, inside the display's clip rect. Call `picocalc.display.flush()` afterwards as usual; you can draw a HUD on top first.

---

#### `picocalc.gfx3d.project(x, y, z)`
- **Returns:** `sx, sy, depth` of a world point on screen, or `nil` if it is behind the near plane. Use it for HUD markers.

---

#### `picocalc.gfx3d.getStats([t])`
- **Returns:** a table describing the last frame. Pass a table `t` to have it filled and returned instead, so a per-frame call allocates nothing:

| Field | Meaning |
|---|---|
| `tris_in` | Triangles submitted |
| `culled` | Skipped by the frustum, back faces, near/far or the viewport (sprites too) |
| `clipped` | Triangles cut by the near plane |
| `drawn` | Triangles rasterised |
| `sprites` | Sprites rasterised |
| `overflow` | Triangles and sprites dropped because the frame held more than 4096, or because a mesh's scratch memory could not be allocated |
| `us_geom` | Microseconds spent in the `draw*` calls |
| `us_raster` | Microseconds spent in `endScene` |

`drawn` counts rasterised triangles; a triangle clipped by the near plane can turn into a quad and be rasterised as two triangles, both counted here.

---

### Textured meshes

A textured mesh has a texture slot and two texture coordinates per vertex, and a colour per vertex instead of per triangle. Its triangles sort with flat ones and sprites in the same scene.

**A textured pixel is the texel times the vertex colour**, interpolated across the triangle, where a channel at its maximum in both (31, or 63 for green) means ×1.5, saturating. So a vertex colour of `0xAD55` leaves a texel about as it is, white makes it half again as bright, and black makes it black. There is no light for textured meshes: bake the lighting into the vertex colours. Fog mixes into the vertex colours (unless the triangle is `NO_FOG`).

Texels are sampled nearest-neighbour and repeat outside 0-1. A triangle draws from level 0 while its longest edge on screen shows under about 2.8 texels per pixel, then from one level smaller for each doubling of that. It draws without perspective correction only where its depth varies so little that the picture stays within about a texel of the correct one.

#### `picocalc.gfx3d.addTexture(lgW, lgH, nmips, pixels)`
Adds a texture of `2^lgW × 2^lgH` texels, copying them.

- `lgW`, `lgH` (integers, 0-10): the size as powers of two, so 1 to 1024 texels a side. Textures need not be square.
- `nmips` (integer): 1 for one level, or more for mip levels after it, each half the size of the last, at most `min(lgW, lgH) + 1` in all.
- `pixels` (string): every level's texels, level 0 first, row by row, two bytes each, little-endian RGB565: the format of `img:getPixels()`, so a power-of-two image's pixels can be passed as they are. Its length must be exactly 2 bytes per texel.
- **Returns:** the texture's slot, an integer counting from 0.

An app has at most 64 textures, kept until it exits; each costs 2 bytes a texel of PSRAM.

```lua
local img = picocalc.graphics.image.load(APP_DIR .. "/grass.png")  -- 64 x 64
local grass = g3.addTexture(6, 6, 1, img:getPixels())
```

---

#### `picocalc.gfx3d.scrollTexture(slot, du, dv)`
Adds `du`, `dv` (in repeats of the texture) to the texture coordinates of every triangle that uses it, when the scene is drawn. It is an offset, not a step: it replaces the last one and lasts across scenes. To move water, pass a total that grows with the frame number, such as `scrollTexture(water, 0, frame * 0.004)`. The last call before `endScene()` applies to the whole scene.

---

#### `picocalc.gfx3d.newTexturedMesh(desc)`
Builds a textured mesh from a table, copying its arrays:

| Field | |
|---|---|
| `xyz` | Flat `{x1, y1, z1, x2, ...}`, at most 4096 vertices |
| `uv` | Two numbers per vertex `{u1, v1, u2, ...}`, in repeats of the texture |
| `color` | One RGB565 colour per vertex, or one integer for every vertex |
| `idx` | Flat 1-based vertex indices, three per triangle, counter-clockwise from the front; at most 8192 triangles |
| `tex` | A texture slot per triangle, or one for every triangle. Each must already exist |
| `flags` | Optional: `BLEND`, `DOUBLE_SIDED` and `NO_FOG` added together, per triangle or one for all |

- **Returns:** a textured mesh. It costs 22 bytes a vertex and 8 a triangle of PSRAM and is freed when collected. While it is built, a copy of the same size waits on the Lua heap until the next collection, so building one needs about twice that for a moment, besides your tables. The first textured mesh an app draws (or its first `scrollTexture`) also allocates about 105 KB for the scene's textured records: 24 bytes for each of the 4096, and 6.7 KB.

```lua
local floor = g3.newTexturedMesh {
    xyz = { -10, 0, 10,  10, 0, 10,  10, 0, -10,  -10, 0, -10 },
    uv = { 0, 0,  10, 0,  10, 10,  0, 10 },  -- the texture ten times each way
    color = 0xAD55,                          -- the texels as they are
    idx = { 1, 2, 3,  1, 3, 4 },
    tex = grass,
}
```

---

#### `texturedMesh:getInfo()`
- **Returns:** `nverts, ntris, cx, cy, cz, radius`, as `mesh:getInfo()` does.

---

#### `picocalc.gfx3d.drawTextured(mesh, x, y, z, yaw, pitch, roll [, scale [, bias]])`
Draws one instance of a textured mesh, as `draw` does a flat one. A textured scene reads the mesh's texture coordinates when `endScene()` draws it, so **the scene keeps the mesh** until the next `beginScene()`: a mesh you drop after drawing it is collected only after that. A frame holds 4096 triangles and sprites, as for flat meshes; a triangle the near plane cuts takes one per piece.

---

#### `picocalc.gfx3d.setSkyImage(slot, horizonRow)` / `picocalc.gfx3d.setSkyImage(nil)`
Fills the background with a texture instead of sky bands or the clear colour, until `setSkyImage(nil)`. Its texels are copied a texel per pixel, with no colour multiplied, from level 0. `horizonRow` (0 to the texture's height) is its first row below the horizon.

- The image follows the camera's yaw and pitch: at the centre of the view it moves as fast as distant scenery, a whole texel at a time, and repeats across. Roll is not followed: the image stays level.
- Rows above and below the texture repeat its top and bottom rows.
- Precisely: the column at screen `x` is `(x − viewport x − round(yaw · focal)) mod width`, where `focal = viewport height / 2 / tan(fovY / 2)` is pixels per radian, and the row at screen `y` is `horizonRow + y − horizonY`, where `horizonY` is the screen row of the true horizon. After `lookAt` the yaw is the view's direction in −π to π, so the image jumps where that wraps; `setCamera`'s yaw can go on past π.

---

#### `picocalc.gfx3d.setSkyRows(y0, y1)`
Limits the rows the sky or the clear colour fills at `endScene()` to `y0` to `y1`, inclusive (none if `y1 < y0`). Triangles and sprites still draw inside the clip rect. A range that holds the viewport is the default: `setSkyRows(0, 319)` undoes it.

---

#### `picocalc.gfx3d.setMirror(on [, planeY])`
While on, everything drawn is reflected in the horizontal plane `y = planeY` (default 0): meshes, textured meshes, background scenery and sprites (flipped upside down). Each triangle faces the eye as its reflection does, and meshes keep the real world's light. It is taken when each thing is drawn, and the sky, bands or image, mirrors if it is on at `endScene()`. `project` gives where the reflection lands. It stays on, across scenes, until `setMirror(false)`, which restores the camera exactly.

---

#### `picocalc.gfx3d.setSpriteFog(on)`
Off by default. On, with the scene's fog on, each sprite's pixels mix towards the fog colour by the sprite's depth, as a triangle's colour does. The fog colour is the one set when `endScene()` runs.

---

### Reflections

A reflecting frame is two scenes: the reflection, drawn into the water's rows only, then the frame itself, with the water blended over the reflection.

```lua
local disp = picocalc.display
-- wy0..wy1: the screen rows the water covers this frame, from project() of its edges.
-- Each scene's endScene fills its sky rows with the sky (bands or image), else with
-- the colour its beginScene was given.
disp.setClipRect(0, wy0, 320, wy1 - wy0 + 1)
g3.setSkyRows(wy0, wy1)
g3.setMirror(true, waterY)
g3.beginScene(skyColor)
-- draw what stands above the water and could show in it
g3.endScene()

disp.clearClipRect()
g3.setSkyRows(0, wy0 - 1)
g3.setMirror(false)
g3.beginScene(skyColor)
-- draw everything, the water's triangles flagged BLEND
g3.endScene()
disp.flush()
```

- Leave out of the second scene whatever lies wholly under the water (a river bed): drawn after the reflection, it would cover it.
- The reflection costs about as much as what it draws: mirror only what is tall enough to show. On the device a reflection took a quarter to a third of a reflecting frame in a native app's valley scene ([Native Loading](Native-Loading.md#reflections) has the figures).

---

### State timing
Camera, projection, light, fog, viewport and mirror settings apply to the `draw*` calls made after they are set — changing one mid-frame affects only later calls in that same frame, not ones already made. The sky is the exception: it is filled once, at `endScene()`, from the camera as it is at that point, not as it was during the frame's `draw` calls. So are the sky rows, the sky image, sprite fog's colour and texture scrolling: the values at `endScene()` apply.

---

### Errors
`draw`, `drawBasis`, `drawBackground`, `drawSprite`, `drawTextured` and `endScene` raise when called outside `beginScene()`/`endScene()`. `setFog` raises unless `0 <= near < far`. `lookAt` raises when `eye` equals `target`, or when `up` is parallel to the view direction. `drawBasis` raises when `forward` or `up` is the zero vector, or when they are parallel. `addTexture` raises on a size or level count out of range, on a string of the wrong length, and when the app has 64 textures already; `scrollTexture`, `setSkyImage` and `newTexturedMesh` raise on a slot `addTexture` has not returned, and `newTexturedMesh` names the field it found wrong. Texture slots are identifiers, and `lgW`, `lgH` and `nmips` are not sizes but exponents and a count that lay out the pixel string: like byte counts, all of them must be exact integers (`addTexture(5.5, ...)` raises rather than rounding, as no rounding could make the string's length right).

---

### Constants
| Constant | Meaning |
|---|---|
| `picocalc.gfx3d.DOUBLE_SIDED` | Draw the triangle's back too, lit as its own side |
| `picocalc.gfx3d.UNLIT` | Ignore the light: always full colour (flat meshes only: a textured mesh has no light) |
| `picocalc.gfx3d.NO_FOG` | Ignore fog |
| `picocalc.gfx3d.BLEND` | Draw the textured triangle 50% over what is behind it, such as water (textured meshes only) |

### Performance notes
- **Submit nearest objects first.** A frame holds 4096 triangles and sprites; the rest are dropped and counted in `overflow`, so submitting near-first means the dropped ones are the distant ones.
- **Each `draw` costs one Lua→C call, plus work per vertex and per visible triangle.** Group static scenery into a few meshes rather than many tiny ones, and draw meshes built in world coordinates with one `drawList` call: on the device each `draw` call adds roughly 20-65 µs of overhead, so 70 of them cost several milliseconds a frame.
- **Allocate meshes once, not every frame.** For smooth frame times, create tables up front and consider `collectgarbage("generational")`.
- **Textured frames are mostly fill.** Time goes on the pixels drawn, a texel read for each, so large near triangles and big sprites cost more than many small far ones. Use mip levels for textures seen at a distance.
- **`endScene()` borrows fast memory.** While no video player exists, the renderer draws the scene on a stack in the video decoder's SRAM, with fogged sprites mixed through tables kept there and magnified sprites' repeated rows copied rather than read again. Creating a video player gives that up until it is collected; the picture is the same either way.

Measured on the device (RP2350 at 200 MHz, a 320×240 viewport with sky and fog, a track strip in 8-segment chunks):

| Triangles submitted | Drawn (avg) | CPU per frame (avg) | of which `draw` calls | of which `endScene` |
|---|---|---|---|---|
| 204 | 135 | 7340 µs | 2291 µs | 3519 µs |
| 402 | 219 | 10379 µs | 4129 µs | 3928 µs |
| 804 | 449 | 17406 µs | 7997 µs | 4836 µs |
| 1602 | 855 | 29171 µs | 15238 µs | 6346 µs |

A full-screen `display.flush()` takes about 16.4 ms of DMA (overlapped with the next frame's work), so 30 fps leaves about 33 ms of CPU per frame.

The "CPU per frame" column is the whole timed loop — `lookAt`, `beginScene`, the Lua loop over `draw` calls, `endScene`, and any OS service work that lands inside the frame (for example serial polling by a test harness) — so it exceeds the sum of the `draw`-calls and `endScene` columns, which are gfx3d's own work.

### Example
```lua
local g3, disp = picocalc.gfx3d, picocalc.display
local cube = g3.newMesh(
    { -1,-1,-1, 1,-1,-1, 1,1,-1, -1,1,-1, -1,-1,1, 1,-1,1, 1,1,1, -1,1,1 },
    { 1,3,2, 1,4,3,  5,6,7, 5,7,8,  1,2,6, 1,6,5,  4,8,7, 4,7,3,  2,3,7, 2,7,6,  1,5,8, 1,8,4 },
    disp.rgb(90, 160, 255))
g3.lookAt(0, 2, 6, 0, 0, 0)
local a = 0
while true do
    picocalc.input.update()
    if picocalc.input.getButtonsPressed() & picocalc.input.BTN_ESC ~= 0 then return end
    a = a + 0.03
    g3.beginScene(disp.BLACK)
    g3.draw(cube, 0, 0, 0, a, a * 0.7, 0)
    g3.endScene()
    disp.flush()
end
```
