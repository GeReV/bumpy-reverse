/* host.h — pure-DOS platform layer (BUMPY_PLAYABLE). Shared decls + hardware
 * register constants. RECONSTRUCTION FIDELITY: this whole layer is the documented
 * platform leaves IMPLEMENTED (the Devilution model); the faithful default build
 * links game_stubs.c instead. See docs/reconstruction-fidelity.md. */
#ifndef HOST_H
#define HOST_H
#include "bumpy.h"
/* VGA */
#define VGA_SEG_PAGE0   0xA000u      /* page 0 plane window */
#define VGA_SEG_PAGE1   0xA200u      /* page 1 (== A000 + 0x200 paragraphs = +0x2000 B) */
#define VGA_SEG_PAGE0_LIN ((u32)VGA_SEG_PAGE0 << 4) /* page 0 as a 20-bit linear address */
#define VGA_DISPLAY_EXTENT 0x4000u   /* both pages' combined per-plane extent [0..0x4000) */
#define VGA_PLANE_BYTES 0x1F40u      /* 320x200/8 per plane per page */
#define VGA_ROW_BYTES   40u
#define SCREEN_W_TILES  20u          /* 320px / 16px-per-tile */
#define SCREEN_H_TILES  25u          /* 200px / 8px-per-tile  */
#define SEQ_INDEX       0x3C4u
#define SEQ_DATA        0x3C5u
#define SEQ_MAP_MASK    0x02u
#define CRTC_INDEX      0x3D4u
#define CRTC_DATA       0x3D5u
#define CRTC_START_HI   0x0Cu
#define CRTC_START_LO   0x0Du
/* Graphics Controller — used by the real-VGA plane-store primitives below. */
#define GC_INDEX        0x3CEu
#define GC_DATA         0x3CFu
#define GC_SET_RESET        0x00u  /* index 0: set/reset                          */
#define GC_ENABLE_SET_RESET 0x01u  /* index 1: enable set/reset                   */
#define GC_DATA_ROTATE      0x03u  /* index 3: data rotate / logical function     */
#define GC_READ_MAP         0x04u  /* index 4: read-map-select (plane read-back)  */
#define GC_MODE             0x05u  /* index 5: write mode / read mode             */
#define GC_BIT_MASK         0x08u  /* index 8: per-bit write enable (RMW masking) */
#define GC_BIT_MASK_ALL     0xFFu  /* GC_BIT_MASK value: all bits writable        */
#define SEQ_MAP_ALL_PLANES  0x0Fu  /* SEQ_DATA (map-mask) value: all 4 planes     */
/* Attribute Controller (palette-index remap) — index+data share one port; the
 * VGA_INPUT_STATUS1 read before writing resets its internal address flip-flop. */
#define ATTR_PORT       0x3C0u
/* DAC (palette RGB) index/data ports. */
#define DAC_INDEX       0x3C8u
#define DAC_DATA        0x3C9u
/* VGA Input Status Register 1 — polled for vertical retrace (bit 3) in vblank sync. */
#define VGA_INPUT_STATUS1 0x3DAu
#define VGA_VRETRACE_BIT  0x08u
/* Flat-plane stride for the host work buffer (= framebuffer plane stride).
 * HOST_FB_16K (playable EXE only, -d on the play build) shrinks each plane to 16 KB so
 * the 4-plane framebuffer is 64 KB instead of 256 KB — the 192 KB freed is what lets
 * level_alloc_buffers' ~167 KB of level/bank/dg buffers fit under the 640 KB DOS limit
 * (the host only ever draws page0, so 16 KB/plane = page0 8 KB + page1 8 KB is enough).
 * The default build + the offline ctests never define HOST_FB_16K → full 64 KB plane,
 * so their objects (and the blitter validation) are byte-for-byte unchanged. */
#ifdef HOST_FB_16K
#define HOST_PLANE_SIZE 0x4000UL     /* 16 KB/plane → 64 KB framebuffer (playable) */
#else
#define HOST_PLANE_SIZE 0x10000UL    /* the reconstructed blitters' flat-plane stride */
#endif
/* the host framebuffer (flat 4-plane RAM image the blitters compose into) */
extern u8 __huge *host_framebuffer;  /* 4 * HOST_PLANE_SIZE */
void host_fb_init(void);             /* allocate + register the page table into it */

/* ── Real-VGA mode-0x0D plane-store primitives (faithful blit target) ──────────
 * The original engine blitters (1cec sprite codec, 1ab9 graphics-overlay tile/copy) write the
 * VGA plane window directly via the Sequencer Map-Mask + GC Bit-Mask + the 4 plane
 * latches; the reconstructed blitters compute the identical per-byte plane values
 * and coverage mask, and these helpers commit them to real VGA (the a000 64KB
 * window) instead of the flat back-buffer.  `off` is a byte offset into that window
 * and already folds the draw page (page0 → off, page1 → 0x2000+off, since
 * a200:0 == a000:2000).  Mode 0x0D is write-mode 0: GC Bit-Mask picks writable
 * bits, the latches (loaded by a dummy read) supply the kept bits, Map-Mask the
 * plane.  Each helper sets Bit-Mask itself so they are call-order independent. */
void host_vga_rmw4(u16 off, u8 v0, u8 v1, u8 v2, u8 v3, u8 bm); /* (v&bm)|(old&~bm) per plane */
void host_vga_put4(u16 off, u8 v0, u8 v1, u8 v2, u8 v3);        /* opaque 4-plane byte write   */
void host_vga_read4(u16 off, u8 *v0, u8 *v1, u8 *v2, u8 *v3);   /* read 4 planes (save-under)  */
void host_vga_clear4(u16 off);                                  /* zero all 4 planes at off    */
void host_vga_blit_end(void);                                   /* restore Bit-Mask=FF, Map=0F */
void host_vga_clear_display(void);                              /* clear a000 [0..0x4000) black */
void host_vga_reset_gc(void);                                   /* GC -> default write state    */
void host_set_draw_page(u8 index);                              /* select draw page (page table) */
u16  host_draw_page_off(void);       /* current draw page as a000-window byte offset (0/0x2000) */
u16  host_page_off_of(u8 index);     /* page offset of table[index] (descriptor word00/word0e)  */
void host_page_table_swap(void);     /* present_frame: engine 1ab9:06c1 table[0]<->[1] swap     */
void host_screens_buf_init(void);    /* host_resource.c — back fullscreen_buf for the resource loader */
/* Menu / level-select cursor (FLECHE.BIN) — the screen-sprite path the host adds so the
 * menu cursor arrow renders (engine resource 9 -> DAT_6c2c).  See host_render.c /
 * host_resource.c. */
void host_load_cursor_bank(void);    /* host_resource.c — load+transform FLECHE.BIN at boot */
void host_cursor_bind(u8 __huge *bank, u32 base_lin, u16 ftbl_off, u16 ftbl_seg); /* host_render.c */
void host_blit_cursor(u16 x, u16 y); /* host_render.c — blit cursor frame 0 at (x,y) */
/* graphics-overlay text (engine 1000:9837/9804 -> overlay 1ab9:1441/13ec; font = DDFNT2.CAR loaded
 * by load_graphics_resources as resource 4, bound at DGROUP 0x68a2).  See host_render.c. */
void host_load_font(void);           /* host_resource.c — load DDFNT2.CAR at boot */
const u8 __far *host_font_ptr(void); /* host_resource.c — the loaded font object (or NULL) */
void host_text_set_pos(u16 x, u16 y);              /* host_render.c — DGROUP 0x6942/0x6944 */
void host_text_set_color(u8 fg, u8 bg);            /* host_render.c — the 1ab9:14ef fg/bg
                                                      expansions (DGROUP 0x68a6/0x68ae)   */
void host_text_draw_string(u16 str_off, u16 str_seg); /* host_render.c — glyph walk+blit  */
/* Runtime DGROUP segment.  The engine's screens/anim/spawn/game leaves write the
 * static DGROUP segment (Ghidra renders DS as 0x203b) into descriptor far-ptr seg
 * fields; in the recompiled image the loaded DGROUP segment differs, so the playable
 * build resolves the *_DGROUP_RUNTIME_SEG macros to this instead of the static 0x203b. */
#define ENGINE_STATIC_DGROUP_SEG 0x203bu
u16 host_dgroup_seg(void);
/* Timer */
extern volatile unsigned host_tick;  /* ISR-incremented frame counter */
void host_timer_teardown(void);      /* restore old INT8 vector + BIOS PIT divisor */
/* Keyboard (INT9) */
void host_keyboard_isr_install(void); /* save old INT9 vector + install host ISR */
void restore_keyboard_isr(void);      /* restore old INT9 vector (teardown) */
#endif
