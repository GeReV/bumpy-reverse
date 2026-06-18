# Open Watcom 16-bit DOS build. Run from src/ after sourcing ow-env.sh.
CC      = wcc
CFLAGS  = -ml -bt=dos -zq -wx
LINK    = wcl
LFLAGS  = -ml -bt=dos -zq
OUTDIR  = ../local/build/src
TARGET  = $(OUTDIR)/BVEC.EXE
SPRITE  = $(OUTDIR)/BSPRITE.EXE
BUMPY   = $(OUTDIR)/BUMPY.EXE

OBJS = bvec.obj bvec_buf1.obj bvec_buf2.obj globals.obj prng.obj dosio.obj vec.obj op12.obj video.obj
SPR_OBJS = bsprite.obj bsprite_buf.obj sprite.obj dosio.obj

# BUMPY.EXE link set (Phase-1 Task 3):
#   main.obj        — reconstructed main + init stub (this task)
#   render modules  — vec/op12/video/sprite/sprite_anim/sprite_chain/sprite_blit/bg_render/entity/bgi_overlay
#   support modules — dosio/prng/globals
#   buffer modules  — bvec_buf1/bvec_buf2 (large __far buffers for VEC decode arena + planar buf)
# Modules not yet implemented (input/player/level) are deferred; their
# symbols are either not referenced from the stub main or will be added
# in later tasks.
BUMPY_OBJS = main.obj &
             vec.obj op12.obj video.obj &
             sprite.obj sprite_anim.obj sprite_chain.obj sprite_blit.obj &
             bg_render.obj entity.obj bgi_overlay.obj &
             dosio.obj prng.obj globals.obj &
             bvec_buf1.obj bvec_buf2.obj

.EXTENSIONS:
.EXTENSIONS: .exe .obj .c

all: $(TARGET) $(SPRITE) $(BUMPY) sprite_blit.obj sprite_chain.obj sprite_anim.obj bg_render.obj entity.obj

$(TARGET): $(OBJS)
	mkdir -p $(OUTDIR) && $(LINK) $(LFLAGS) -fe=$(TARGET) $(OBJS)

$(SPRITE): $(SPR_OBJS)
	mkdir -p $(OUTDIR) && $(LINK) $(LFLAGS) -fe=$(SPRITE) $(SPR_OBJS)

$(BUMPY): $(BUMPY_OBJS)
	mkdir -p $(OUTDIR) && $(LINK) $(LFLAGS) -fe=$(BUMPY) $(BUMPY_OBJS)

.c.obj:
	$(CC) $(CFLAGS) -fo=$^&.obj $^&.c

clean: .SYMBOLIC
	rm -f *.obj $(TARGET) $(SPRITE) $(BUMPY)
