---
title: "API 3D"
---

## picocalc.gfx3d

A small retained-mode 3D renderer for flat-shaded, low-poly scenes: triangle meshes, a camera, one directional light, distance fog, horizon-aligned sky bands, background scenery and camera-facing sprites.

Your Lua code builds meshes once and then, every frame, sets the camera and calls `draw` for each object between `beginScene()` and `endScene()`. Transform, culling, lighting, fog, clipping and depth ordering all run in C. `draw` does its work at once and keeps no reference to the mesh.

Objects are drawn far to near (painter's order), with no depth buffer. Keep triangles reasonably short, so that long faces don't sort in front of objects above them. Use `bias` or `sortAsOne` for objects that sit on other geometry.

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
| Fog | Off |
| Sky | Off |

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
Sets the direction **towards** the light and the ambient level (0–1, default 0.25). Each face is lit by `ambient + (1 − ambient) · max(0, n·l)`.

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

#### `picocalc.gfx3d.drawBackground(mesh)`
Draws distant scenery, such as a mountain ring, centred on the camera. It turns with the camera but never moves with it. It is never fogged or far-clipped, and it is drawn before everything else, in the order submitted.

---

#### `picocalc.gfx3d.drawSprite(image, x, y, z, size [, sx, sy, sw, sh [, bias]])`
Draws a camera-facing image centred at `(x, y, z)`, `size` world units tall, sorted with the triangles.

- `sx, sy, sw, sh` choose a source rectangle, such as a frame of a sheet.
- The image's transparent colour is honoured.
- The image stays alive until the next `beginScene()`.

---

#### `picocalc.gfx3d.endScene()`
Fills the viewport (sky or clear colour) and draws everything, far to near, into the back buffer. Call `picocalc.display.flush()` afterwards as usual; you can draw a HUD on top first.

---

#### `picocalc.gfx3d.project(x, y, z)`
- **Returns:** `sx, sy, depth` of a world point on screen, or `nil` if it is behind the near plane. Use it for HUD markers.

---

#### `picocalc.gfx3d.getStats()`
- **Returns:** a table describing the last frame:

| Field | Meaning |
|---|---|
| `tris_in` | Triangles submitted |
| `culled` | Skipped by the frustum, back faces, near/far or the viewport (sprites too) |
| `clipped` | Triangles cut by the near plane |
| `drawn` | Triangles rasterised |
| `sprites` | Sprites rasterised |
| `overflow` | Triangles and sprites dropped because the frame held more than 4096 |
| `us_geom` | Microseconds spent in the `draw*` calls |
| `us_raster` | Microseconds spent in `endScene` |

---

### Constants
| Constant | Meaning |
|---|---|
| `picocalc.gfx3d.DOUBLE_SIDED` | Draw the triangle's back too, lit as its own side |
| `picocalc.gfx3d.UNLIT` | Ignore the light: always full colour |
| `picocalc.gfx3d.NO_FOG` | Ignore fog |

### Performance notes
- **Submit nearest objects first.** A frame holds 4096 triangles and sprites; the rest are dropped and counted in `overflow`, so submitting near-first means the dropped ones are the distant ones.
- **Each `draw` costs one Lua→C call, plus work per vertex and per visible triangle.** Group static scenery into a few meshes rather than many tiny ones.
- **Allocate meshes once, not every frame.** For smooth frame times, create tables up front and consider `collectgarbage("generational")`.

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
