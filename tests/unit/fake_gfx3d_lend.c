// What mp3_player.c needs of gfx3d: the MP3 tests link no renderer.
int g_gfx3d_take_backs;
void gfx3d_sram_take_back(void);
void gfx3d_sram_take_back(void) { g_gfx3d_take_backs++; }
