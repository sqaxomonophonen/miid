CXXFLAGS+=-std=c++11
CXXFLAGS+=-Wno-narrowing
LDLIBS+=-lm -pthread
CXXFLAGS+=-O0 -g
#CXXFLAGS+=-Wall

PKGS=sdl2 gl fluidsynth
CXXFLAGS+=$(shell pkg-config --cflags ${PKGS})
LDLIBS+=$(shell pkg-config --libs ${PKGS})

all: miid

binfont.c: font.ttf
	xxd -i $^ $@

IMGUI_OBJS=imgui.o imgui_widgets.o imgui_tables.o imgui_draw.o imgui_impl_sdl2.o imgui_impl_opengl2.o

miid.o: miid.cpp config.h
config.o: config.cpp config.h

miid: main_sdl2_opengl2.o miid.o config.o binfont.o stb_ds.o $(IMGUI_OBJS)
	$(CXX) $^ $(LDLIBS) -o $@

clean:
	rm -f *.o miid
