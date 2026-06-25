/* config_screens.h — boot graphics/sound device select screens (config_screens.c).
 *
 * Reconstructions of two real engine functions: gfx_driver_init (1ab9:02ce) and
 * sound_device_select_screen (202c:0000).  Built only into the playable image
 * (BUMPY_PLAYABLE); the default BUMPY.EXE boot does not call them. */
#ifndef CONFIG_SCREENS_H
#define CONFIG_SCREENS_H

#ifdef BUMPY_PLAYABLE
void gfx_driver_init(void);            /* 1ab9:02ce — graphics-adapter select (sets palette_mode) */
void sound_device_select_screen(void); /* 202c:0000 — sound-device select (sets sound_device_state) */
#endif

#endif /* CONFIG_SCREENS_H */
