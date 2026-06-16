# Open Watcom 16-bit DOS build. Run from src/ after sourcing ow-env.sh.
CC      = wcc
CFLAGS  = -ml -bt=dos -zq -wx
LINK    = wcl
LFLAGS  = -ml -bt=dos -zq
OUTDIR  = ../local/build/src
TARGET  = $(OUTDIR)/BVEC.EXE

OBJS = bvec.obj globals.obj

.EXTENSIONS:
.EXTENSIONS: .exe .obj .c

all: $(TARGET)

$(TARGET): $(OBJS)
	mkdir -p $(OUTDIR) && $(LINK) $(LFLAGS) -fe=$(TARGET) $(OBJS)

.c.obj:
	$(CC) $(CFLAGS) -fo=$^&.obj $^&.c

clean: .SYMBOLIC
	rm -f *.obj $(TARGET)
