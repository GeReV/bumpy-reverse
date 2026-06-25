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

# BUMPY.EXE link set (Phase-1 Task 7 — full module integration):
#   main.obj        — reconstructed entry point (calls the real session)
#   session/loop    — game.obj (run_game_session/init_game_session_state/
#                     reset_game_state/game_loop) + game_stubs.obj (faithful-
#                     signature stubs for the not-yet-reconstructed per-tick
#                     callees — linkability scaffolding, DEFERRED Phase 2)
#   gameplay modules— level (start_level) / input (keyboard) / player (move spine)
#   render modules  — vec/op12/video/sprite/sprite_anim/sprite_chain/sprite_blit/bg_render/entity/bgi_overlay
#   support modules — dosio/prng/globals
#   buffer modules  — bvec_buf1/bvec_buf2 (large __far buffers for VEC decode arena + planar buf)
BUMPY_OBJS = main.obj game.obj game_stubs.obj &
             level.obj input.obj player.obj player2.obj items.obj anim.obj spawn.obj sound.obj screens.obj &
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
	rm -f play/*.obj $(PLAY)

# ── Playable pure-DOS build (Devilution model) ──────────────────────────────
# Swaps game_stubs.obj for the host platform layer (src/host/*.c).
#
# FLAG-REBUILD SCHEME: all playable-build objects go into the play/ subdirectory
# (e.g. play/main.obj, play/host_video.obj).  This means the default BUMPY.EXE
# objects (compiled WITHOUT -dBUMPY_PLAYABLE) never collide with the playable
# objects (compiled WITH -dBUMPY_PLAYABLE).  Both `make BUMPY` and `make play`
# can be run from a clean tree in any order without interference.
#
# The play/ subdir is created by the play target before any object is built.
PLAY    = $(OUTDIR)/BUMPYP.EXE
PLAYDIR = play
PLAY_OBJS = &
    $(PLAYDIR)/main.obj $(PLAYDIR)/game.obj $(PLAYDIR)/game_stubs.obj &
    $(PLAYDIR)/host_video.obj $(PLAYDIR)/host_render.obj $(PLAYDIR)/host_timer.obj &
    $(PLAYDIR)/host_input.obj $(PLAYDIR)/host_view.obj $(PLAYDIR)/host_boot.obj &
    $(PLAYDIR)/host_resource.obj $(PLAYDIR)/host_config_screens.obj &
    $(PLAYDIR)/level.obj $(PLAYDIR)/input.obj $(PLAYDIR)/player.obj $(PLAYDIR)/player2.obj &
    $(PLAYDIR)/items.obj $(PLAYDIR)/anim.obj $(PLAYDIR)/spawn.obj $(PLAYDIR)/sound.obj &
    $(PLAYDIR)/screens.obj &
    $(PLAYDIR)/vec.obj $(PLAYDIR)/op12.obj $(PLAYDIR)/video.obj &
    $(PLAYDIR)/sprite.obj $(PLAYDIR)/sprite_anim.obj $(PLAYDIR)/sprite_chain.obj &
    $(PLAYDIR)/sprite_blit.obj &
    $(PLAYDIR)/bg_render.obj $(PLAYDIR)/entity.obj $(PLAYDIR)/bgi_overlay.obj &
    $(PLAYDIR)/dosio.obj $(PLAYDIR)/prng.obj $(PLAYDIR)/globals.obj &
    $(PLAYDIR)/bvec_buf1.obj $(PLAYDIR)/bvec_buf2.obj

# Stack size: the Watcom default DOS stack is 2 KB (-k not given), too small for the
# playable build's real call depth + the 500 Hz host INT8 ISR frames — the Task-9 boot
# bring-up overflowed it (SP ran below __STACKLOW into BSS, CS:IP fell off the end of
# DGROUP).  DGROUP data is already ~47 KB, so the stack must fit in the remaining
# 64 KB room: 0x4000 (16 KB) → DGROUP ~63 KB, under the 64 KB group limit.
# Default build (BUMPY target above) is untouched, so its byte-image is unchanged.
PLAY_STACK = 0x4000

play: .SYMBOLIC $(PLAY_OBJS)
	mkdir -p $(OUTDIR) && $(LINK) $(LFLAGS) -k$(PLAY_STACK) -fe=$(PLAY) $(PLAY_OBJS)

# host TUs — compile WITH -dBUMPY_PLAYABLE from host/ subdir.
$(PLAYDIR)/host_video.obj: host/host_video.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_video.obj host/host_video.c
$(PLAYDIR)/host_render.obj: host/host_render.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_render.obj host/host_render.c
$(PLAYDIR)/host_timer.obj: host/host_timer.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_timer.obj host/host_timer.c
$(PLAYDIR)/host_input.obj: host/host_input.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_input.obj host/host_input.c
$(PLAYDIR)/host_view.obj: host/host_view.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_view.obj host/host_view.c
$(PLAYDIR)/host_boot.obj: host/host_boot.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_boot.obj host/host_boot.c
$(PLAYDIR)/host_resource.obj: host/host_resource.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_resource.obj host/host_resource.c
$(PLAYDIR)/host_config_screens.obj: host/host_config_screens.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/host_config_screens.obj host/host_config_screens.c

# Shared TUs — compile WITH -dBUMPY_PLAYABLE into play/ (default BUMPY objs unaffected).
$(PLAYDIR)/main.obj: main.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/main.obj main.c
$(PLAYDIR)/game.obj: game.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/game.obj game.c
$(PLAYDIR)/game_stubs.obj: game_stubs.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/game_stubs.obj game_stubs.c
$(PLAYDIR)/level.obj: level.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/level.obj level.c
$(PLAYDIR)/input.obj: input.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/input.obj input.c
$(PLAYDIR)/player.obj: player.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/player.obj player.c
$(PLAYDIR)/player2.obj: player2.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/player2.obj player2.c
$(PLAYDIR)/items.obj: items.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/items.obj items.c
$(PLAYDIR)/anim.obj: anim.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/anim.obj anim.c
$(PLAYDIR)/spawn.obj: spawn.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/spawn.obj spawn.c
$(PLAYDIR)/sound.obj: sound.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/sound.obj sound.c
$(PLAYDIR)/screens.obj: screens.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/screens.obj screens.c
$(PLAYDIR)/vec.obj: vec.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/vec.obj vec.c
$(PLAYDIR)/op12.obj: op12.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/op12.obj op12.c
$(PLAYDIR)/video.obj: video.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/video.obj video.c
$(PLAYDIR)/sprite.obj: sprite.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/sprite.obj sprite.c
$(PLAYDIR)/sprite_anim.obj: sprite_anim.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/sprite_anim.obj sprite_anim.c
$(PLAYDIR)/sprite_chain.obj: sprite_chain.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/sprite_chain.obj sprite_chain.c
$(PLAYDIR)/sprite_blit.obj: sprite_blit.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/sprite_blit.obj sprite_blit.c
$(PLAYDIR)/bg_render.obj: bg_render.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/bg_render.obj bg_render.c
$(PLAYDIR)/entity.obj: entity.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/entity.obj entity.c
$(PLAYDIR)/bgi_overlay.obj: bgi_overlay.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/bgi_overlay.obj bgi_overlay.c
$(PLAYDIR)/dosio.obj: dosio.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/dosio.obj dosio.c
$(PLAYDIR)/prng.obj: prng.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/prng.obj prng.c
$(PLAYDIR)/globals.obj: globals.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/globals.obj globals.c
$(PLAYDIR)/bvec_buf1.obj: bvec_buf1.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/bvec_buf1.obj bvec_buf1.c
$(PLAYDIR)/bvec_buf2.obj: bvec_buf2.c
	mkdir -p $(PLAYDIR) && $(CC) $(CFLAGS) -dBUMPY_PLAYABLE -fo=$(PLAYDIR)/bvec_buf2.obj bvec_buf2.c
