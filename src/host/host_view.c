#ifdef BUMPY_PLAYABLE
#include "host.h"
/* Skeleton — real bodies land in later tasks. */
/* init_sprite_structs — one-time per-game sprite-struct setup; render-core leaf. */
void init_sprite_structs(void)                              {}
/* init_fullscreen_view_desc — set up the fullscreen view descriptor (mode, flag). */
void init_fullscreen_view_desc(u8 mode, u8 flag)            { (void)mode; (void)flag; }
/* setup_fullscreen_view — per-load fullscreen view/page restore. */
void setup_fullscreen_view(void)                            {}
/* show_text_screen — text-screen present path; render-core leaf. */
void show_text_screen(void)                                 {}
/* show_pause_screen — pause-screen present path; render-core leaf. */
void show_pause_screen(void)                                {}
#endif
