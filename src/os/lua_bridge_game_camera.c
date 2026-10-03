#include "lua_bridge_internal.h"
#include "../drivers/display.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH FB_WIDTH
#define SCREEN_HEIGHT FB_HEIGHT

#define CAMERA_SHAKE_DECAY 8.0f

typedef struct {
    float x, y;
    float target_x, target_y;
    float lag;
    float zoom;
    float shake_x, shake_y;
    float shake_time;
    float shake_duration;
    float shake_intensity_x;
    float shake_intensity_y;
    bool has_bounds;
    float bounds_min_x, bounds_min_y, bounds_max_x, bounds_max_y;
    bool has_target;
} lua_camera_t;

#define CAMERA_MT "picocalc.game.camera"

static lua_camera_t *check_camera(lua_State *L, int idx) {
    return (lua_camera_t *)lb_checkudata(L, idx, CAMERA_MT);
}

static int l_camera_new(lua_State *L) {
    lua_camera_t *cam = (lua_camera_t *)lua_newuserdata(L, sizeof(lua_camera_t));
    memset(cam, 0, sizeof(lua_camera_t));
    cam->zoom = 1.0f;
    luaL_getmetatable(L, CAMERA_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_camera_setPosition(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->x = (float)luaL_checknumber(L, 2);
    cam->y = (float)luaL_checknumber(L, 3);
    return 0;
}

static int l_camera_move(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->x += (float)luaL_checknumber(L, 2);
    cam->y += (float)luaL_checknumber(L, 3);
    return 0;
}

static int l_camera_getPosition(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    lua_pushnumber(L, cam->x);
    lua_pushnumber(L, cam->y);
    return 2;
}

static int l_camera_setZoom(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->zoom = (float)luaL_checknumber(L, 2);
    if (cam->zoom < 0.1f) cam->zoom = 0.1f;
    if (cam->zoom > 10.0f) cam->zoom = 10.0f;
    return 0;
}

static int l_camera_getZoom(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    lua_pushnumber(L, cam->zoom);
    return 1;
}

static int l_camera_setTarget(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->target_x = (float)luaL_checknumber(L, 2);
    cam->target_y = (float)luaL_checknumber(L, 3);
    cam->lag = (float)luaL_optnumber(L, 4, 0.15f);
    cam->has_target = true;
    return 0;
}

static int l_camera_clearTarget(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->has_target = false;
    return 0;
}

static int l_camera_setBounds(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->bounds_min_x = (float)luaL_checknumber(L, 2);
    cam->bounds_min_y = (float)luaL_checknumber(L, 3);
    cam->bounds_max_x = (float)luaL_checknumber(L, 4);
    cam->bounds_max_y = (float)luaL_checknumber(L, 5);
    cam->has_bounds = true;
    return 0;
}

static int l_camera_clearBounds(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->has_bounds = false;
    return 0;
}

static int l_camera_getBounds(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    if (cam->has_bounds) {
        lua_pushnumber(L, cam->bounds_min_x);
        lua_pushnumber(L, cam->bounds_min_y);
        lua_pushnumber(L, cam->bounds_max_x);
        lua_pushnumber(L, cam->bounds_max_y);
        return 4;
    }
    return 0;
}

static int l_camera_shake(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float intensity = (float)luaL_checknumber(L, 2);
    float duration = (float)luaL_checknumber(L, 3);
    cam->shake_intensity_x = intensity;
    cam->shake_intensity_y = intensity;
    cam->shake_duration = duration;
    cam->shake_time = 0.0f;
    return 0;
}

static int l_camera_shakeX(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float intensity = (float)luaL_checknumber(L, 2);
    float duration = (float)luaL_checknumber(L, 3);
    cam->shake_intensity_x = intensity;
    cam->shake_intensity_y = 0.0f;
    cam->shake_duration = duration;
    cam->shake_time = 0.0f;
    return 0;
}

static int l_camera_shakeY(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float intensity = (float)luaL_checknumber(L, 2);
    float duration = (float)luaL_checknumber(L, 3);
    cam->shake_intensity_x = 0.0f;
    cam->shake_intensity_y = intensity;
    cam->shake_duration = duration;
    cam->shake_time = 0.0f;
    return 0;
}

static int l_camera_stopShake(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    cam->shake_time = cam->shake_duration;
    cam->shake_x = 0.0f;
    cam->shake_y = 0.0f;
    return 0;
}

static int l_camera_worldToScreen(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float wx = (float)luaL_checknumber(L, 2);
    float wy = (float)luaL_checknumber(L, 3);
    float sx = (wx - cam->x) * cam->zoom + SCREEN_WIDTH / 2.0f + cam->shake_x;
    float sy = (wy - cam->y) * cam->zoom + SCREEN_HEIGHT / 2.0f + cam->shake_y;
    lua_pushinteger(L, (int)sx);
    lua_pushinteger(L, (int)sy);
    return 2;
}

static int l_camera_screenToWorld(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float sx = (float)luaL_checknumber(L, 2);
    float sy = (float)luaL_checknumber(L, 3);
    float wx = (sx - SCREEN_WIDTH / 2.0f - cam->shake_x) / cam->zoom + cam->x;
    float wy = (sy - SCREEN_HEIGHT / 2.0f - cam->shake_y) / cam->zoom + cam->y;
    lua_pushnumber(L, wx);
    lua_pushnumber(L, wy);
    return 2;
}

static int l_camera_update(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float dt = (float)luaL_checknumber(L, 2);
    
    if (cam->has_target) {
        float t = 1.0f - expf(-dt / cam->lag);
        cam->x += (cam->target_x - cam->x) * t;
        cam->y += (cam->target_y - cam->y) * t;
    }
    
    if (cam->has_bounds) {
        float hw = SCREEN_WIDTH / (2.0f * cam->zoom);
        float hh = SCREEN_HEIGHT / (2.0f * cam->zoom);
        if (cam->x - hw < cam->bounds_min_x) cam->x = cam->bounds_min_x + hw;
        if (cam->x + hw > cam->bounds_max_x) cam->x = cam->bounds_max_x - hw;
        if (cam->y - hh < cam->bounds_min_y) cam->y = cam->bounds_min_y + hh;
        if (cam->y + hh > cam->bounds_max_y) cam->y = cam->bounds_max_y - hh;
    }
    
    if (cam->shake_time < cam->shake_duration) {
        cam->shake_time += dt;
        float progress = cam->shake_time / cam->shake_duration;
        float decay = 1.0f - progress;
        float intensity = decay * decay;
        cam->shake_x = ((rand() % 200) - 100) / 100.0f * cam->shake_intensity_x * intensity;
        cam->shake_y = ((rand() % 200) - 100) / 100.0f * cam->shake_intensity_y * intensity;
    } else {
        cam->shake_x = 0.0f;
        cam->shake_y = 0.0f;
    }
    
    return 0;
}

static int l_camera_getOffset(lua_State *L) {
    lua_camera_t *cam = check_camera(L, 1);
    float ox = -cam->x * cam->zoom + SCREEN_WIDTH / 2.0f + cam->shake_x;
    float oy = -cam->y * cam->zoom + SCREEN_HEIGHT / 2.0f + cam->shake_y;
    lua_pushinteger(L, (int)ox);
    lua_pushinteger(L, (int)oy);
    return 2;
}

static const luaL_Reg l_camera_methods[] = {
    {"setPosition", l_camera_setPosition},
    {"move", l_camera_move},
    {"getPosition", l_camera_getPosition},
    {"setZoom", l_camera_setZoom},
    {"getZoom", l_camera_getZoom},
    {"setTarget", l_camera_setTarget},
    {"clearTarget", l_camera_clearTarget},
    {"setBounds", l_camera_setBounds},
    {"clearBounds", l_camera_clearBounds},
    {"getBounds", l_camera_getBounds},
    {"shake", l_camera_shake},
    {"shakeX", l_camera_shakeX},
    {"shakeY", l_camera_shakeY},
    {"stopShake", l_camera_stopShake},
    {"worldToScreen", l_camera_worldToScreen},
    {"screenToWorld", l_camera_screenToWorld},
    {"update", l_camera_update},
    {"getOffset", l_camera_getOffset},
    {NULL, NULL}
};

static const luaL_Reg l_camera_lib[] = {
    {"new", l_camera_new},
    {NULL, NULL}
};

void lua_bridge_game_camera_init(lua_State *L) {
    lb_register_type(L, CAMERA_MT, l_camera_methods, NULL);
    
    lua_newtable(L);
    luaL_setfuncs(L, l_camera_lib, 0);
}
