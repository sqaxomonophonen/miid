CFLAGS+=-std=gnu11
CFLAGS+=-Wall

PKGS=sdl2 gl fluidsynth
CFLAGS+=$(shell pkg-config --cflags ${PKGS})
LDLIBS+=$(shell pkg-config --libs ${PKGS})

LDLIBS+=-lm

CFLAGS+=-O0 -g

all: miid

binfont.c: font.ttf
	xxd -i $^ $@

miid.o: miid.c config.h

miid: miid.o nanovg.o nanovg_gl.o binfont.o

clean:
	rm -f *.o miid
