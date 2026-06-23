#ifdef BUMPY_PLAYABLE
#include "host.h"
/* Skeleton — real bodies land in later tasks. */
/* init_timer_resource_table 1000:7bad — timer/resource-table init (off, seg). */
void init_timer_resource_table(u16 off, u16 seg)            { (void)off; (void)seg; }
/* init_joystick_handlers — joystick handler-script install. */
void init_joystick_handlers(void)                           {}
/* mouse_reset — INT33 mouse reset. */
void mouse_reset(void)                                      {}
/* init_sound_tables 1000:7563 — sound table init (off, off, seg). */
void init_sound_tables(u16 a, u16 b, u16 seg)               { (void)a; (void)b; (void)seg; }
/* set_disk_swap_callback 1000:72ef — install INT24 + disk-swap prompt callback. */
void set_disk_swap_callback(u16 int24_handler, u16 callback){ (void)int24_handler; (void)callback; }
/* set_resource_table — point the resource table at (off, seg). */
void set_resource_table(u16 off, u16 seg)                   { (void)off; (void)seg; }
/* reset_opaque_session_globals — opaque ~46-global session reset. */
void reset_opaque_session_globals(void)                     {}
/* load_current_level_data 1000:32b0 — engine standalone level-header loader. */
void load_current_level_data(void)                          {}
#endif
