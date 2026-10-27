#include "lua_bridge_internal.h"

#include <math.h>

// Near plane, in world units in front of the camera (which sits at z = -fov).
// A vertex at or behind it has no sane projection: fov / (fov + z) divides by
// zero on the camera plane and flips sign behind it, so edges are clipped to
// the plane and faces touching such a vertex are culled.
#define NEAR_DEPTH 1.0f
// Projected coordinates are clamped before the float -> int conversion
// (undefined out of range); the line clipper handles anything this large.
#define PROJ_LIMIT 1.0e9f

static int proj_coord(float c, float v, float scale) {
  float p = c + v * scale;
  if (!(p > -PROJ_LIMIT)) p = -PROJ_LIMIT;  // also catches NaN
  if (p > PROJ_LIMIT) p = PROJ_LIMIT;
  return (int)p;
}

// draw3DWireframeEx(verts, edges, angleX, angleY, angleZ, scx, scy, fov,
//                   edgeColor, fillColor, fillMode, vertSize, faces)
//
// Enhanced wireframe with optional filled triangles
// fillMode: 0 = wireframe only, 1 = fill only, 2 = both fill and wireframe
// faces: flat array of vertex indices in triples {v1,v2,v3, v1,v2,v3, ...}
static int l_graphics_draw3DWireframeEx(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    float aX  = (float)luaL_checknumber(L, 3);
    float aY  = (float)luaL_checknumber(L, 4);
    float aZ  = (float)luaL_checknumber(L, 5);
    int   scx = (int)lb_checkint(L, 6);
    int   scy = (int)lb_checkint(L, 7);
    float fov = (float)luaL_checknumber(L, 8);
    uint16_t edge_color = (uint16_t)luaL_checkinteger(L, 9);
    uint16_t fill_color = (uint16_t)luaL_optinteger(L, 10, 0);
    int fill_mode = (int)luaL_optinteger(L, 11, 0);
    int vert_size = (int)lb_optint(L, 12, 3);
    // Arg 13: faces table (optional, flat array of vertex index triples)
    int has_faces = lua_istable(L, 13);

    float cX = cosf(aX), sX = sinf(aX);
    float cY = cosf(aY), sY = sinf(aY);
    float cZ = cosf(aZ), sZ = sinf(aZ);

    float m00 = cZ*cY,               m01 = cZ*sY*sX - sZ*cX,  m02 = cZ*sY*cX + sZ*sX;
    float m10 = sZ*cY,               m11 = sZ*sY*sX + cZ*cX,  m12 = sZ*sY*cX - cZ*sX;
    float m20 = -sY,                 m21 = cY*sX,              m22 = cY*cX;

    int n_verts = (int)lua_rawlen(L, 1) / 3;
    if (n_verts > 64) n_verts = 64;
    int px[64], py[64];
    float rvx[64], rvy[64], rvz[64];  // rotated positions for lighting
    bool vis[64];                     // in front of the near plane
    lb_array_t verts;                 // read in place where it can be
    lb_array_begin(L, 1, &verts);

    for (int i = 0; i < n_verts; i++) {
        float vx = lb_array_number(&verts, i*3 + 1);
        float vy = lb_array_number(&verts, i*3 + 2);
        float vz = lb_array_number(&verts, i*3 + 3);

        float rx = m00*vx + m01*vy + m02*vz;
        float ry = m10*vx + m11*vy + m12*vz;
        float rz = m20*vx + m21*vy + m22*vz;

        rvx[i] = rx;
        rvy[i] = ry;
        rvz[i] = rz;

        float depth = fov + rz;
        vis[i] = depth > NEAR_DEPTH && isfinite(depth) && isfinite(rx) &&
                 isfinite(ry);
        float scale = vis[i] ? fov / depth : 0.0f;
        px[i] = proj_coord((float)scx, rx, scale);
        py[i] = proj_coord((float)scy, ry, scale);
    }

    // Light direction (normalized): upper-right, toward camera
    static const float lx = 0.267f, ly = -0.535f, lz = -0.802f;
    static const float ambient = 0.15f;

    // Fill triangles from faces table with per-face Lambert shading
    if (fill_color != 0 && (fill_mode == 1 || fill_mode == 2) && has_faces) {
        int r5_base = (fill_color >> 11) & 0x1F;
        int g6_base = (fill_color >> 5) & 0x3F;
        int b5_base = fill_color & 0x1F;

        int n_face_indices = (int)lua_rawlen(L, 13);
        lb_array_t faces;
        lb_array_begin(L, 13, &faces);
        for (int i = 1; i + 2 <= n_face_indices; i += 3) {
            int a = (int)lb_array_integer(&faces, i) - 1;
            int b = (int)lb_array_integer(&faces, i + 1) - 1;
            int c = (int)lb_array_integer(&faces, i + 2) - 1;

            if (a < 0 || a >= n_verts || b < 0 || b >= n_verts || c < 0 || c >= n_verts)
                continue;
            if (!vis[a] || !vis[b] || !vis[c])
                continue;  // touches the near plane: culled, not clipped

            // Face normal via cross product of two edges
            float ex1 = rvx[b] - rvx[a], ey1 = rvy[b] - rvy[a], ez1 = rvz[b] - rvz[a];
            float ex2 = rvx[c] - rvx[a], ey2 = rvy[c] - rvy[a], ez2 = rvz[c] - rvz[a];
            float nx = ey1*ez2 - ez1*ey2;
            float ny = ez1*ex2 - ex1*ez2;
            float nz = ex1*ey2 - ey1*ex2;

            // Back-face cull: dot(faceNormal, cameraToVertex)
            // Camera is at (0, 0, -fov), so view vector = (rvx, rvy, rvz + fov)
            // dot > 0 means normal faces away from camera (back face) — skip it
            float cull = nx * rvx[a] + ny * rvy[a] + nz * (rvz[a] + fov);
            if (cull > 0.0f) continue;

            // Lambert shading: dot(N, L) / |N|
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            if (len < 1e-6f) continue;
            float dot = (nx*lx + ny*ly + nz*lz) / len;
            float brightness = dot < ambient ? ambient : (dot > 1.0f ? 1.0f : dot);

            // Scale fill color channels by brightness
            uint16_t shaded = ((int)(r5_base * brightness) << 11)
                            | ((int)(g6_base * brightness) << 5)
                            | (int)(b5_base * brightness);

            display_fill_triangle(px[a], py[a], px[b], py[b], px[c], py[c], shaded);
        }
    }

    // Draw edges if fill_mode is 0 or 2
    if (fill_mode == 0 || fill_mode == 2) {
        int n_edges_flat = (int)lua_rawlen(L, 2);
        lb_array_t edges;
        lb_array_begin(L, 2, &edges);
        for (int i = 1; i <= n_edges_flat; i += 2) {
            int a = (int)lb_array_integer(&edges, i) - 1;
            int b = (int)lb_array_integer(&edges, i + 1) - 1;
            if (a < 0 || a >= n_verts || b < 0 || b >= n_verts)
                continue;
            if (vis[a] && vis[b]) {
                display_draw_line(px[a], py[a], px[b], py[b], edge_color);
            } else if (vis[a] || vis[b]) {
                // Clip the edge where it crosses the near plane.
                int in = vis[a] ? a : b, out = vis[a] ? b : a;
                float din = fov + rvz[in], dout = fov + rvz[out];
                float t = (din - NEAR_DEPTH) / (din - dout);
                float cx = rvx[in] + (rvx[out] - rvx[in]) * t;
                float cy = rvy[in] + (rvy[out] - rvy[in]) * t;
                float scale = fov / NEAR_DEPTH;
                display_draw_line(px[in], py[in],
                                  proj_coord((float)scx, cx, scale),
                                  proj_coord((float)scy, cy, scale),
                                  edge_color);
            }
        }

        // Draw vertex dots
        if (vert_size > 0) {
            int half = vert_size / 2;
            for (int i = 0; i < n_verts; i++)
                if (vis[i])
                    display_fill_rect(px[i] - half, py[i] - half, vert_size, vert_size, edge_color);
        }
    }

    return 0;
}

void lua_bridge_register_3d(lua_State *L) {
    lua_pushcfunction(L, l_graphics_draw3DWireframeEx);
    lua_setglobal(L, "draw3DWireframeEx");
}
