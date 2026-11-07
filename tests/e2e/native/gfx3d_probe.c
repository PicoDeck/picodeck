// native_gfx3d — the gfx3d table (API version 11) from a native app, and the three
// sys entries that came with it. One behaviour per mode; the mode is the app's name
// in app.json, as in fidelity_probe.c, so tests/e2e/test_native_gfx3d.py stages this
// one ELF under a different name per case, on the simulator and on the device.
//
// What a mode finds goes to the log as PROBE lines, and to /data/<id>/probe.txt,
// since serial lines can be dropped on the device. The last line is "PROBE done".
// A mode that draws then waits for the test to read the screen.
//
// The scene is simple enough to work out by hand. fovY 90 degrees, so a point at
// depth 2 lands 80 px from the centre per world unit; texture rows run down the
// screen. No float arithmetic here: this links no libgcc, so a float is passed along
// or logged as its bits.
//
// Built by tests/e2e/native/Makefile into tests/e2e/fixtures/native_gfx3d/main.elf
// (committed, so the E2E job needs no ARM toolchain).
#include <stdarg.h>
#include <stddef.h>

#include "app_abi.h"
#include "os.h"

static const PicoCalcAPI *A;
static const picocalc_gfx3d_t *G;
static const char *s_id;

#define W 320
#define CLEAR_COLOR 0x2104u  // r 4, g 8, b 4
#define NEUTRAL 0xAD55u      // r 21, g 42, b 21: about x1 on a texel
#define KEY 0xF81Fu
#define SPR_COLOR 0x07E0u

// Rows down the screen, columns across.
static const uint16_t TEX4[16] = {
    0xF800, 0x07E0, 0x001F, 0xFFFF,  //
    0x8410, 0xF81F, 0xFFE0, 0x07FF,  //
    0x4208, 0xC618, 0x2945, 0xA514,  //
    0x0000, 0x7800, 0x03E0, 0x000F,
};

// --- Output ---------------------------------------------------------------

static char s_out[4096];
static int s_out_n;

static void out_char(char *line, int *n, char c) {
    if (*n < 159) line[(*n)++] = c;
}

static void out_str(char *line, int *n, const char *s) {
    while (*s) out_char(line, n, *s++);
}

// %d %x %s, all this needs. The device's printf is newlib-nano's and the simulator's
// log has its own, smaller, formatter; both print a whole line with %s.
static void p(const char *fmt, ...) {
    char line[160];
    int n = 0;
    va_list ap;
    va_start(ap, fmt);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') {
            out_char(line, &n, *f);
            continue;
        }
        f++;
        if (*f == 's') {
            const char *s = va_arg(ap, const char *);
            out_str(line, &n, s ? s : "(null)");
        } else if (*f == 'd') {
            int v = va_arg(ap, int);
            unsigned u = (unsigned)v;
            if (v < 0) {
                out_char(line, &n, '-');
                u = 0u - u;
            }
            char d[12];
            int k = 0;
            do {
                d[k++] = (char)('0' + u % 10u);
                u /= 10u;
            } while (u);
            while (k) out_char(line, &n, d[--k]);
        } else {  // x
            unsigned v = va_arg(ap, unsigned);
            for (int sh = 28; sh >= 0; sh -= 4)
                out_char(line, &n, "0123456789abcdef"[(v >> sh) & 15u]);
        }
    }
    va_end(ap);
    line[n] = '\0';
    A->sys->log("%s", line);
    for (int i = 0; i < n && s_out_n < (int)sizeof(s_out) - 1; i++)
        s_out[s_out_n++] = line[i];
    if (s_out_n < (int)sizeof(s_out) - 1)
        s_out[s_out_n++] = '\n';
}

static void path_of(char *path, const char *name) {
    int n = 0;
    for (const char *s = "/data/"; *s; s++) path[n++] = *s;
    for (const char *s = s_id; *s; s++) path[n++] = *s;
    if (name) {
        path[n++] = '/';
        for (const char *s = name; *s; s++) path[n++] = *s;
    }
    path[n] = '\0';
}

// The results file, written last so that a test finding it finds all of it.
static void done(void) {
    p("PROBE done");
    char path[128];
    A->fs->mkdir("/data");
    path_of(path, NULL);
    A->fs->mkdir(path);
    path_of(path, "probe.txt");
    pcfile_t f = A->fs->open(path, "w");
    if (f) {
        A->fs->write(f, s_out, s_out_n);
        A->fs->close(f);
    }
}

// Serve the OS until the test asks the app to leave.
static void hold(void) {
    for (;;) {
        A->sys->poll();
        if (A->sys->shouldExit() || (A->input->getButtonsPressed() & BTN_ESC))
            return;
    }
}

static bool is(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static uint32_t bits(float f) {
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

static void log_stats(pc3d_t g) {
    pc3d_stats_t s;
    G->getStats(g, &s);
    p("PROBE stats tris_in=%d culled=%d clipped=%d drawn=%d sprites=%d overflow=%d pixels=%d",
      (int)s.tris_in, (int)s.culled, (int)s.clipped, (int)s.drawn, (int)s.sprites,
      (int)s.overflow, (int)s.pixels);
}

// --- Building blocks ------------------------------------------------------

static const float ZERO[3] = {0.0f, 0.0f, 0.0f};

static pc3d_t context(int capacity) {
    pc3d_t g = G->create(capacity);
    if (g) G->setProjection(g, 1.5707964f, 1.0f, 100.0f);
    return g;
}

// x0..x1, y0..y1 at depth -z, facing the camera (or away, `away`). uv: (0, 0) at the
// top left of the screen.
static pc3dmesh_t quad_ex(float x0, float y0, float x1, float y1, float z, uint8_t tex,
                          uint8_t flags, uint16_t color, bool away) {
    const float xyz[12] = {x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z};
    const float uv[8] = {0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    const uint16_t col[4] = {color, color, color, color};
    const uint16_t front[6] = {0, 1, 2, 0, 2, 3}, back[6] = {0, 2, 1, 0, 3, 2};
    const uint8_t texs[2] = {tex, tex}, fl[2] = {flags, flags};
    pc3d_mesh_desc_t d;
    d.nverts = 4;
    d.ntris = 2;
    d.xyz = xyz;
    d.uv = uv;
    d.color = col;
    d.idx = away ? back : front;
    d.tex = texs;
    d.flags = fl;
    return G->meshNew(&d);
}

static pc3dmesh_t quad(float x0, float y0, float x1, float y1, float z, uint8_t tex,
                       uint8_t flags, uint16_t color) {
    return quad_ex(x0, y0, x1, y1, z, tex, flags, color, false);
}

// One texel of one colour, so a pixel reads as that colour times the vertex's.
static int solid(pc3d_t g, uint16_t color) {
    return G->texAdd(g, 0, 0, 1, &color);
}

// 8 x 8: a ring of SPR_COLOR around a keyed middle.
static int sprite_image(pc3d_t g) {
    uint16_t px[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            px[y * 8 + x] = (x >= 2 && x < 6 && y >= 2 && y < 6) ? KEY : SPR_COLOR;
    return G->imageAdd(g, 8, 8, KEY, px);
}

static void draw_mesh(pc3d_t g, pc3dmesh_t m) {
    G->drawMesh(g, m, NULL, ZERO, 1.0f, 0.0f);
}

// --- Modes that draw ------------------------------------------------------

// A texture on a quad over x, y 80..240, one texel 40 px across, and a sprite over
// 260..300 x 20..60. Returned so the leak modes can skip the free.
static pc3d_t scene(pc3dmesh_t *mesh) {
    pc3d_t g = context(0);
    const int tex = G->texAdd(g, 2, 2, 1, TEX4);
    const int img = sprite_image(g);
    pc3dmesh_t m = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)tex, 0, NEUTRAL);
    const pc3d_sprite_t s = {1.5f, 1.5f, -2.0f, 0.5f, (uint32_t)img};
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, m);
    const int filed = G->drawSprites(g, &s, 1, 0.0f);
    G->end(g);
    p("PROBE made ctx=%d tex=%d img=%d mesh=%d filed=%d", g != NULL, tex, img, m != NULL, filed);
    *mesh = m;
    return g;
}

static void mode_draw(void) {
    pc3dmesh_t m;
    pc3d_t g = scene(&m);
    log_stats(g);
    p("PROBE version=%d", (int)A->version);
    p("PROBE firmware=%s", A->sys->getFirmwareId());
    p("PROBE clock=%d", (int)A->sys->getClockKhz());
    A->display->flush();
    done();
    hold();
    G->meshFree(m);
    G->destroy(g);
}

// Everything left behind, for the loader.
static void mode_leak(void) {
    pc3dmesh_t m;
    pc3d_t g = scene(&m);
    (void)g;
    done();
}

static void mode_leak_midscene(void) {
    pc3d_t g = context(0);
    pc3dmesh_t m = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(g, 0xFFFFu), 0, NEUTRAL);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, m);
    done();
}

// The mesh older than the context: the loader frees newest first, so here the
// context goes first, with records of the mesh still in it.
static void mode_leak_mesh_first(void) {
    const uint16_t px = 0xFFFFu;
    const float xyz[9] = {-1.0f, -1.0f, -2.0f, 1.0f, -1.0f, -2.0f, 0.0f, 1.0f, -2.0f};
    static const float uv[6] = {0.0f};
    const uint16_t col[3] = {NEUTRAL, NEUTRAL, NEUTRAL}, idx[3] = {0, 1, 2};
    const uint8_t texs[1] = {0};
    pc3d_mesh_desc_t d;
    d.nverts = 3;
    d.ntris = 1;
    d.xyz = xyz;
    d.uv = uv;
    d.color = col;
    d.idx = idx;
    d.tex = texs;
    d.flags = NULL;
    pc3dmesh_t m = G->meshNew(&d);
    pc3d_t g = context(0);
    G->texAdd(g, 0, 0, 1, &px);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, m);
    done();
}

// Everything freed by the app, the context first, with the second free of each ignored.
static void mode_free_all(void) {
    pc3dmesh_t m;
    pc3d_t g = scene(&m);
    G->destroy(g);
    G->destroy(g);
    G->meshFree(m);
    G->meshFree(m);
    G->destroy(NULL);
    G->meshFree(NULL);
    p("PROBE freed");
    done();
}

// Raw pixels before begin and after end stay: the back buffer's order is byte-swapped.
static uint16_t raw(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

static void raw_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c) {
    for (int r = y; r < y + h; r++)
        for (int i = x; i < x + w; i++)
            fb[r * W + i] = raw(c);
}

static void mode_raw_end(void) {
    pc3d_t g = context(0);
    pc3dmesh_t m = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(g, 0xFFFFu), 0, NEUTRAL);
    A->display->clear(PC_COLOR_BLACK);
    uint16_t *fb = A->display->getBackBuffer();
    raw_rect(fb, 0, 0, 10, 10, PC_COLOR_BLUE);
    G->begin(g, false, 0);
    draw_mesh(g, m);
    G->end(g);
    raw_rect(fb, 100, 100, 10, 10, PC_COLOR_RED);
    A->display->flush();
    done();
    hold();
}

static void mode_clip(void) {
    pc3d_t g = context(0);
    pc3dmesh_t m = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(g, 0xFFFFu), 0, NEUTRAL);
    A->display->clear(PC_COLOR_BLACK);
    A->display->setClipRect(0, 0, 160, 320);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, m);
    G->end(g);
    A->display->clearClipRect();
    A->display->flush();
    done();
    hold();
}

// Depth 2 with fog 1..3: half way, on vertex colours, a NO_FOG quad beside it keeps
// its own, and a sprite takes it only with sprite fog on.
static void mode_fog(void) {
    pc3d_t g = context(0);
    const int tex = solid(g, 0xFFFFu);
    const int img = sprite_image(g);
    pc3dmesh_t foggy = quad(-1.0f, -1.0f, 0.0f, 1.0f, -2.0f, (uint8_t)tex, 0, NEUTRAL);
    pc3dmesh_t clear = quad(0.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)tex, PC3D_TM_NO_FOG, NEUTRAL);
    const pc3d_sprite_t plain = {-1.5f, 1.5f, -2.0f, 0.5f, (uint32_t)img};
    const pc3d_sprite_t fogged = {1.5f, 1.5f, -2.0f, 0.5f, (uint32_t)img};
    G->setFog(g, true, 1.0f, 3.0f, 0xF800u);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, foggy);
    draw_mesh(g, clear);
    G->drawSprites(g, &plain, 1, 0.0f);
    G->setSpriteFog(g, true);
    G->drawSprites(g, &fogged, 1, 0.0f);
    G->end(g);
    A->display->flush();
    done();
    hold();
}

// 8 x 8: red is the column times 4, green the row times 8, blue full.
static void mode_sky(void) {
    pc3d_t g = context(0);
    uint16_t px[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            px[y * 8 + x] = (uint16_t)(((unsigned)(x * 4) << 11) | ((unsigned)(y * 8) << 5) | 31u);
    const int tex = G->texAdd(g, 3, 3, 1, px);
    p("PROBE sky badslot=%d badrow=%d ok=%d", G->setSkyImage(g, tex + 1, 4),
      G->setSkyImage(g, tex, 9), G->setSkyImage(g, tex, 4));
    G->setSkyRows(g, 0, 170);
    A->display->clear(PC_COLOR_BLACK);
    G->begin(g, false, 0);
    G->end(g);
    A->display->flush();
    done();
    hold();
}

static void mode_scroll(void) {
    pc3d_t g = context(0);
    const int tex = G->texAdd(g, 2, 2, 1, TEX4);
    pc3dmesh_t m = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)tex, 0, NEUTRAL);
    G->texScroll(g, tex, 0.25f, 0.5f);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, m);
    G->end(g);
    A->display->flush();
    done();
    hold();
}

// y 0.5..1.5 about a plane at 0.25 lands on y -1..0: rows 160..240, not 40..120.
static void mode_mirror(void) {
    pc3d_t g = context(0);
    pc3dmesh_t m = quad(-1.0f, 0.5f, 1.0f, 1.5f, -2.0f, (uint8_t)solid(g, 0xFFFFu), 0, NEUTRAL);
    G->setMirror(g, true, 0.25f);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, m);
    G->end(g);
    A->display->flush();
    done();
    hold();
}

// Three solid quads, all facing away but the first: BLEND over the clear colour, the
// second drawn because of its flag, the third dropped for want of it.
static void mode_flags(void) {
    pc3d_t g = context(0);
    const uint8_t tex = (uint8_t)solid(g, 0xFFFFu);
    pc3dmesh_t blend = quad(-1.5f, -0.5f, -0.75f, 0.5f, -2.0f, tex, PC3D_TM_BLEND, NEUTRAL);
    pc3dmesh_t two = quad_ex(-0.5f, -0.5f, 0.25f, 0.5f, -2.0f, tex, PC3D_TM_DOUBLE_SIDED, NEUTRAL, true);
    pc3dmesh_t one = quad_ex(0.5f, -0.5f, 1.25f, 0.5f, -2.0f, tex, 0, NEUTRAL, true);
    G->begin(g, true, CLEAR_COLOR);
    draw_mesh(g, blend);
    draw_mesh(g, two);
    draw_mesh(g, one);
    G->end(g);
    log_stats(g);
    A->display->flush();
    done();
    hold();
}

// The same depth: submission order would put b on top, a's bias of -1 puts it there. Over
// them, a sprite with bias -1 shows and one with bias +1 does not.
static void mode_bias(void) {
    pc3d_t g = context(0);
    pc3dmesh_t a = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(g, 0xF800u), 0, 0xFFFFu);
    pc3dmesh_t b = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(g, 0x001Fu), 0, 0xFFFFu);
    const int img = sprite_image(g);
    const pc3d_sprite_t front = {-0.5f, 0.0f, -2.0f, 0.5f, (uint32_t)img};
    const pc3d_sprite_t behind = {0.5f, 0.0f, -2.0f, 0.5f, (uint32_t)img};
    G->begin(g, true, CLEAR_COLOR);
    G->drawMesh(g, a, NULL, ZERO, 1.0f, -1.0f);
    draw_mesh(g, b);
    G->drawSprites(g, &front, 1, -1.5f);
    G->drawSprites(g, &behind, 1, 1.5f);
    G->end(g);
    A->display->flush();
    done();
    hold();
}

// x 0..1, y 0..0.5 at the origin, scaled by 0.5, turned 90 degrees about z and moved to
// (0.5, 0, -2): x 0.25..0.5, y 0..0.5, so screen x 180..200, y 120..160.
static void mode_xform(void) {
    pc3d_t g = context(0);
    pc3dmesh_t m = quad(0.0f, 0.0f, 1.0f, 0.5f, 0.0f, (uint8_t)solid(g, 0xFFFFu), 0, NEUTRAL);
    const float rot[9] = {0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const float pos[3] = {0.5f, 0.0f, -2.0f};
    G->begin(g, true, CLEAR_COLOR);
    G->drawMesh(g, m, rot, pos, 0.5f, 0.0f);
    G->end(g);
    A->display->flush();
    done();
    hold();
}

// Two contexts in turn: the second's begin takes the shared block from the first
// mid-scene, and both scenes still paint.
static void mode_two(void) {
    pc3d_t a = context(0), b = context(0);
    pc3dmesh_t ma = quad(-1.0f, -1.0f, 0.0f, 1.0f, -2.0f, (uint8_t)solid(a, 0xF800u), 0, 0xFFFFu);
    pc3dmesh_t mb = quad(0.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(b, 0x001Fu), 0, 0xFFFFu);
    G->begin(a, true, CLEAR_COLOR);
    draw_mesh(a, ma);
    G->begin(b, false, 0);
    draw_mesh(b, mb);
    G->end(a);
    G->end(b);
    A->display->flush();
    done();
    hold();
}

// --- Modes that log -------------------------------------------------------

static void log_project(const char *what, pc3d_t g, float x, float y, float z) {
    const float pt[3] = {x, y, z};
    float sx = 0.0f, sy = 0.0f, depth = 0.0f;
    const bool ok = G->project(g, pt, &sx, &sy, &depth);
    p("PROBE project %s ok=%d %x %x %x", what, ok, (unsigned)bits(sx), (unsigned)bits(sy),
      (unsigned)bits(depth));
}

static void mode_calls(void) {
    p("PROBE create bad=%d,%d", G->create(-1) != NULL, G->create(0x8000) != NULL);
    pc3d_t g = G->create(0);
    p("PROBE projection bad=%d,%d,%d ok=%d", G->setProjection(g, 3.5f, 1.0f, 100.0f),
      G->setProjection(g, 1.0f, -1.0f, 100.0f), G->setProjection(g, 1.0f, 2.0f, 1.0f),
      G->setProjection(g, 1.5707964f, 1.0f, 100.0f));
    log_project("origin", g, 1.0f, 1.0f, -2.0f);
    log_project("near", g, 0.0f, 0.0f, -0.5f);

    const float pos[3] = {1.0f, 2.0f, 3.0f};
    G->setCamera(g, pos, 0.3f, 0.2f, 0.1f);
    log_project("camera", g, 0.5f, -0.5f, -4.0f);

    const float eye[3] = {3.0f, 1.0f, 2.0f}, target[3] = {-1.0f, 0.5f, -2.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    p("PROBE lookat at_eye=%d along_up=%d ok=%d", G->lookAt(g, eye, eye, up),
      G->lookAt(g, up, ZERO, up), G->lookAt(g, eye, target, up));
    log_project("lookat", g, -1.0f, 0.5f, -2.0f);
    log_project("lookat_off", g, 0.0f, 1.0f, -1.0f);

    static const uint16_t px[21] = {0};
    p("PROBE tex bad=%d,%d,%d,%d,%d", G->texAdd(g, -1, 2, 1, px), G->texAdd(g, 11, 2, 1, px),
      G->texAdd(g, 2, 2, 0, px), G->texAdd(g, 2, 2, 4, px), G->texAdd(g, 2, 2, 1, NULL));
    const int first_tex = G->texAdd(g, 2, 2, 1, px);
    const int mip_tex = G->texAdd(g, 2, 2, 3, px);
    p("PROBE tex first=%d mips=%d", first_tex, mip_tex);
    p("PROBE img bad=%d,%d,%d first=%d", G->imageAdd(g, 0, 2, 0, px), G->imageAdd(g, 2, 1025, 0, px),
      G->imageAdd(g, 2, 2, 0, NULL), G->imageAdd(g, 2, 2, 0, px));

    // A description with one thing wrong each.
    const float xyz[9] = {-1.0f, -1.0f, -2.0f, 1.0f, -1.0f, -2.0f, 0.0f, 1.0f, -2.0f};
    const float uv[6] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const uint16_t col[3] = {NEUTRAL, NEUTRAL, NEUTRAL};
    const uint16_t idx[3] = {0, 1, 2}, bad_idx[3] = {0, 1, 3};
    const uint8_t tex[1] = {0};
    pc3d_mesh_desc_t d;
    d.nverts = 3;
    d.ntris = 1;
    d.xyz = xyz;
    d.uv = uv;
    d.color = col;
    d.idx = idx;
    d.tex = tex;
    d.flags = NULL;
    pc3dmesh_t good = G->meshNew(&d);
    d.nverts = 0;
    const bool no_verts = G->meshNew(&d) != NULL;
    d.nverts = 3;
    d.idx = bad_idx;
    const bool index = G->meshNew(&d) != NULL;
    d.idx = idx;
    d.uv = NULL;
    const bool no_uv = G->meshNew(&d) != NULL;
    p("PROBE mesh good=%d bad=%d,%d,%d,%d", good != NULL, no_verts, index, no_uv,
      G->meshNew(NULL) != NULL);

    p("PROBE sprites none=%d,%d", G->drawSprites(g, NULL, 4, 0.0f),
      G->drawSprites(g, (const pc3d_sprite_t *)xyz, 0, 0.0f));
    G->meshFree(good);
    G->destroy(g);

    // One record of room: the quad's second triangle counts as overflow.
    pc3d_t small = context(1);
    pc3dmesh_t q = quad(-1.0f, -1.0f, 1.0f, 1.0f, -2.0f, (uint8_t)solid(small, 0xFFFFu), 0, NEUTRAL);
    G->begin(small, true, CLEAR_COLOR);
    draw_mesh(small, q);
    G->end(small);
    log_stats(small);

    // More sprites than the simulator's trampoline reads at a time.
    pc3d_t wide = context(1000);
    const int img = sprite_image(wide);
    static pc3d_sprite_t many[700];
    for (int i = 0; i < 700; i++) {
        many[i].x = 0.0f;
        many[i].y = 0.0f;
        many[i].z = -2.0f;
        many[i].size = 0.5f;
        many[i].slot = (uint32_t)img;
    }
    many[699].slot = 99;  // no such image: skipped
    G->begin(wide, true, CLEAR_COLOR);
    p("PROBE sprites many=%d", G->drawSprites(wide, many, 700, 0.0f));
    log_stats(wide);
    G->destroy(wide);
    G->meshFree(q);
    G->destroy(small);
    done();
}

// For the idle dim: 30 frames a second, so the simulator's virtual clock moves. The
// line says the dim timeout has passed; the test then reads the backlight.
static void mode_idle(bool reset) {
    A->perf->setTargetFPS(30);
    const uint32_t t0 = A->sys->getTimeMs();
    bool said = false;
    for (;;) {
        if (reset) A->sys->resetIdleTimer();
        A->sys->poll();
        if (A->sys->shouldExit()) return;
        A->perf->endFrame();
        if (!said && A->sys->getTimeMs() - t0 > 70000u) {
            said = true;
            A->sys->log("PROBE idle passed");
        }
    }
}

__attribute__((noinline)) static void run_mode(const char *m) {
    if (is(m, "draw")) mode_draw();
    else if (is(m, "leak")) mode_leak();
    else if (is(m, "leak_midscene")) mode_leak_midscene();
    else if (is(m, "leak_mesh_first")) mode_leak_mesh_first();
    else if (is(m, "free_all")) mode_free_all();
    else if (is(m, "raw_end")) mode_raw_end();
    else if (is(m, "clip")) mode_clip();
    else if (is(m, "fog")) mode_fog();
    else if (is(m, "sky")) mode_sky();
    else if (is(m, "scroll")) mode_scroll();
    else if (is(m, "mirror")) mode_mirror();
    else if (is(m, "flags")) mode_flags();
    else if (is(m, "bias")) mode_bias();
    else if (is(m, "xform")) mode_xform();
    else if (is(m, "two")) mode_two();
    else if (is(m, "calls")) mode_calls();
    else if (is(m, "idle_plain")) mode_idle(false);
    else if (is(m, "idle_reset")) mode_idle(true);
    else A->sys->log("PROBE no mode %s", m);
}

void picodeck_main(const PicoCalcAPI *api, const char *app_dir, const char *app_id,
                   const char *app_name) {
    (void)app_dir;
    A = api;
    s_id = app_id;
    if (api->version < 11) {
        api->sys->log("PROBE version=%d", (int)api->version);
        return;
    }
    G = api->gfx3d;
    run_mode(app_name);
}
