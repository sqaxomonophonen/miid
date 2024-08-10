CXXFLAGS+=-std=c++11
CXXFLAGS+=-Wno-narrowing
LDLIBS+=-lm -pthread
CXXFLAGS+=-O0 -g
#CXXFLAGS+=-Wall

all: miid

binfont.c: font.ttf
	xxd -i $^ $@

IMGUI_OBJS=imgui.o imgui_widgets.o imgui_tables.o imgui_draw.o imgui_impl_sdl2.o imgui_impl_opengl2.o

miid.o: miid.cpp config.h
config.o: config.cpp config.h

OBJS=main_sdl2_opengl2.o miid.o config.o binfont.o stb_ds.o
miid: $(OBJS) $(IMGUI_OBJS)
	$(CXX) $(OBJS) $(IMGUI_OBJS) $(LDLIBS) -o $@

clean:
	rm -f *.o miid
