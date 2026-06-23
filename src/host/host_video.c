#ifdef BUMPY_PLAYABLE
#include "host.h"
/* Skeleton — real bodies land in Tasks 3-4. */
void present_frame(u8 page)          { (void)page; }
void clear_viewport(void)            {}
void set_display_page(u8 page)       { (void)page; }
void apply_level_palette(void)       {}
void set_palette_mode(u8 m, u8 f)    { (void)m; (void)f; }
void init_crtc_window(u16 a,u16 b,u16 c,u16 d){ (void)a;(void)b;(void)c;(void)d; }
void init_display_97a4(void)         {}
void init_display_97f1(void)         {}
#endif
