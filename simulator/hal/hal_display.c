// HAL Display - SDL2 Implementation

#include "hal_display.h"
#include <SDL2/SDL.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "web_platform.h"
#endif

static SDL_Window* g_window = NULL;
static SDL_Renderer* g_renderer = NULL;
static SDL_Texture* g_texture = NULL;
static uint16_t* g_framebuffer = NULL;
static _Atomic uint32_t g_present_count = 0;

bool hal_display_init(const char* title) {
    // Create window
    // Note: SDL_WINDOW_SHOWN can cause issues with dummy driver
    g_window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        SDL_WINDOW_ALLOW_HIGHDPI
    );
    
    if (!g_window) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        return false;
    }
    
    // Create renderer with vsync for 60fps cap
    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);

    // Headless environments (SDL_VIDEODRIVER=dummy in CI) register no
    // accelerated render driver, and SDL only matches drivers whose flags are a
    // superset of those requested — so SDL_RENDERER_ACCELERATED excludes the
    // software renderer outright. Fall back to it rather than failing to start.
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (!g_renderer) {
        fprintf(stderr, "Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        return false;
    }
    
    // Create texture for framebuffer (RGB565 format)
    g_texture = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT);
    
    if (!g_texture) {
        fprintf(stderr, "Failed to create texture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_renderer = NULL;
        g_window = NULL;
        return false;
    }
    
    // Allocate framebuffer
    g_framebuffer = (uint16_t*)malloc(DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    if (!g_framebuffer) {
        fprintf(stderr, "Failed to allocate framebuffer\n");
        SDL_DestroyTexture(g_texture);
        SDL_DestroyRenderer(g_renderer);
        SDL_DestroyWindow(g_window);
        g_texture = NULL;
        g_renderer = NULL;
        g_window = NULL;
        return false;
    }
    
    // Clear to black
    memset(g_framebuffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    
    // Set scaling quality (nearest neighbor for pixel-perfect scaling)
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    
    printf("[Display] Initialized %dx%d (scale %dx)\n", 
           DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_SCALE);
    
    return true;
}

void hal_display_shutdown(void) {
    if (g_framebuffer) {
        free(g_framebuffer);
        g_framebuffer = NULL;
    }
    
    if (g_texture) {
        SDL_DestroyTexture(g_texture);
        g_texture = NULL;
    }
    
    if (g_renderer) {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    
    if (g_window) {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    
    printf("[Display] Shutdown\n");
}

uint16_t* hal_display_get_framebuffer(void) {
    return g_framebuffer;
}

uint32_t hal_display_get_present_count(void) {
    return g_present_count;
}

void hal_display_present(void) {
    if (!g_framebuffer || !g_texture || !g_renderer) return;
    g_present_count++;
    
    // Update texture with framebuffer data
    SDL_UpdateTexture(g_texture, NULL, g_framebuffer, DISPLAY_WIDTH * sizeof(uint16_t));
    
    // Clear renderer
    SDL_RenderClear(g_renderer);
    
    // Copy texture to renderer (with scaling)
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    
    // Present
    SDL_RenderPresent(g_renderer);

#ifdef __EMSCRIPTEN__
    // Desktop PRESENTVSYNC blocks ~16 ms here; the browser does not, so pace
    // to ~60 fps and let the browser composite the frame.
    static double s_last_present_ms;
    double now = emscripten_get_now();
    double wait = 16.0 - (now - s_last_present_ms);
    web_yield(wait > 0 ? (uint32_t)wait : 0);
    s_last_present_ms = emscripten_get_now();
#endif
}

void hal_display_clear(void) {
    if (g_framebuffer) {
        memset(g_framebuffer, 0, DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
    }
}

void hal_display_set_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < DISPLAY_WIDTH && y >= 0 && y < DISPLAY_HEIGHT && g_framebuffer) {
        g_framebuffer[y * DISPLAY_WIDTH + x] = color;
    }
}
