#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>

#include <SDL.h>
#include "gl.h"

#include <fluidsynth.h>

#include "nanovg.h"
#include "nanovg_gl.h"
//#include "nanovg_gl_utils.h"

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#define N_NOTES (1<<7)

enum midi_event_type {
	NOTE_OFF             = 0x80, // [key, velocity]
	NOTE_ON              = 0x90, // [key, velocity]
	POLY_AFTERTOUCH      = 0xa0, // [key, pressure]
	CONTROL_CHANGE       = 0xb0, // [controller, value]
	PROGRAM_CHANGE       = 0xc0, // [program]
	CHANNEL_AFTERTOUCH   = 0xd0, // [pressure]
	PITCH_BEND           = 0xe0, // [lsb7, msb7] 14-bit pitch value: lsb7+(msb7<<7)
	SYSEX                = 0xf7,
	META                 = 0xff,
};

enum meta_type {
	TEXT              = 0x01,
	TRACK_NAME        = 0x03,
	INSTRUMENT_NAME   = 0x04,
	MARKER            = 0x06,
	MIDI_CHANNEL      = 0x20,
	END_OF_TRACK      = 0x2f,
	SET_TEMPO         = 0x51,
	SMPTE_OFFSET      = 0x54,
	TIME_SIGNATURE    = 0x58,
	KEY_SIGNATURE     = 0x59,
	CUSTOM            = 0x7f,
};

enum cc_type {
	MODULATION_WHEEL       = 1,
	VOLUME                 = 7,
	PAN                    = 10,
	DAMPER_PEDAL           = 64,
	EFFECT1_DEPTH          = 91,
	RESET_ALL_CONTROLLERS  = 121,
};

#define EMIT_KITS                \
	KIT(STANDARD_KIT, 1)     \
	KIT(ROOM_KIT, 9)         \
	KIT(POWER_KIT, 17)       \
	KIT(ELECTRONIC_KIT, 25)  \
	KIT(TR_808_KIT, 26)      \
	KIT(JAZZ_KIT, 33)        \
	KIT(BRUSH_KIT, 41)       \
	KIT(ORCHESTRA_KIT, 49)   \
	KIT(SOUND_FX_KIT, 57)    \

enum kit_program {
	#define KIT(NAME,ID) NAME = ID,
	EMIT_KITS
	#undef KIT
};

struct soundfont {
	char* path;
	int error;
};

struct {
	int cfgsz;
	int using_audio;
	SDL_Window* window;
	int true_screen_width;
	int true_screen_height;
	int screen_width;
	int screen_height;
	float pixel_ratio;
	NVGcontext* vg;
	SDL_AudioDeviceID audio_device;
	fluid_synth_t* fluid_synth;
	struct soundfont* soundfont_arr;
	struct mid* myd;
} g;

struct {
	int current_soundfont_index;
} state;

static void refresh_soundfont(void)
{
	if (!g.using_audio) return;

	const int n = arrlen(g.soundfont_arr);
	if (state.current_soundfont_index >= n) {
		state.current_soundfont_index = 0;
	} else if (state.current_soundfont_index < 0) {
		state.current_soundfont_index = n-1;
	}
	if (state.current_soundfont_index < 0) {
		return;
	}
	struct soundfont* sf = &g.soundfont_arr[state.current_soundfont_index];
	if (sf->error) return;

	const int handle = fluid_synth_sfload(g.fluid_synth, sf->path, /*reset_presets=*/1);
	if (handle == FLUID_FAILED) {
		fprintf(stderr, "WARNING: failed to load SoundFont %s\n", sf->path);
		sf->error = 1;
		return;
	}
	fprintf(stderr, "INFO: loaded SoundFont #%d %s\n",
		state.current_soundfont_index,
		sf->path);
}

static void populate_screen_globals(void)
{
	//int prev_width = g.true_screen_width;
	//int prev_height = g.true_screen_height;
	SDL_GL_GetDrawableSize(g.window, &g.true_screen_width, &g.true_screen_height);
	int w, h;
	SDL_GetWindowSize(g.window, &w, &h);
	g.pixel_ratio = (float)g.true_screen_width / (float)w;
	g.screen_width = g.true_screen_width / g.pixel_ratio;
	g.screen_height = g.true_screen_height / g.pixel_ratio;
}

// via binfont.c
extern unsigned char font_ttf[];
extern unsigned int font_ttf_len;

NVGcontext* nvgCreateGL2(int flags);

static void audio_callback(void* usr, Uint8* stream, int len)
{
	memset(stream, 0, len);
	const int n_frames = len / (2*sizeof(float));
	fluid_synth_write_float(g.fluid_synth,
		n_frames,
		stream, 0, 2,
		stream, 1, 2);
}

static char* copystring(char* src)
{
	const size_t sz = strlen(src);
	char* dst = malloc(sz+1);
	memcpy(dst, src, sz);
	dst[sz] = 0;
	return dst;
}

#define ARRAY_LENGTH(xs) (sizeof(xs) / sizeof((xs)[0]))

struct mev {
	int pos;
	unsigned char b[4];
};

struct trk {
	char* title;
	struct mev* mev_arr;
};

struct mid {
	char* text;
	int division;
	struct trk* trk_arr;
};

static int read_counter;

static int read_u8(FILE* f)
{
	int ch = fgetc(f);
	if (ch == EOF) return -1;
	read_counter++;
	return ch;
}

static void unread_u8(FILE* f)
{
	assert(fseek(f, -1, SEEK_CUR) == 0);
	read_counter--;
}

static int skip_magic_string(FILE* f, char* s)
{
	size_t remaining = strlen(s);
	char* p = s;
	while (remaining > 0) {
		if (read_u8(f) != *(p++)) return 0;
		remaining--;
	}
	return 1;
}

static int skip_n(FILE* f, int n)
{
	while (n > 0) {
		if (read_u8(f) == -1) return 0;
		n--;
	}
	return 1;
}

static char* read_string(FILE* f, int n)
{
	char* p0 = malloc(n+1);
	char* p = p0;
	while (n > 0) {
		int c = read_u8(f);
		if (c == -1) {
			free(p0);
			return NULL;
		}
		*(p++) = c;
		n--;
	}
	return p0;
}

static int read_i32_be(FILE* f)
{
	unsigned char s[4];
	if (fread(s, ARRAY_LENGTH(s), 1, f) != 1) return -1;
	return
		(((int)(s[0])) << 24) +
		(((int)(s[1])) << 16) +
		(((int)(s[2])) << 8)  +
		 ((int)(s[3]));
}

static int read_u16_be(FILE* f)
{
	unsigned char s[2];
	if (fread(s, ARRAY_LENGTH(s), 1, f) != 1) return -1;
	return
		(((int)(s[0])) << 8)  +
		 ((int)(s[1]));
}

static int read_midi_varuint(FILE* f)
{
	int v = 0;
	for (;;) {
		int ch = read_u8(f);
		if (ch == -1) return -1;
		v |= (ch & 0x7f);
		if (!(ch & 0x80)) break;
		v <<= 7;
	}
	return v;
}

static struct mid* mid_load(char* path)
{
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "ERROR: %s: %s\n", path, strerror(errno));
		return NULL;
	}
	if (!skip_magic_string(f, "MThd")) {
		fprintf(stderr, "ERROR: %s: bad header (fourcc)\n", path);
		return NULL;
	}
	if (read_i32_be(f) != 6) {
		fprintf(stderr, "ERROR: %s: bad header (MThd size not 6)\n", path);
		return NULL;
	}

	const int format = read_u16_be(f);
	if (format != 1) {
		fprintf(stderr, "ERROR: %s: unsupported format %d; only format 1 is supported, sorry!\n", path, format);
		return NULL;
	}

	const int n_tracks = read_u16_be(f);
	if (n_tracks == 0) {
		fprintf(stderr, "ERROR: %s: MIDI file has no tracks; aborting load\n", path);
		return NULL;
	}

	const int division = read_u16_be(f);
	if (division >= 0x8000) {
		fprintf(stderr, "ERROR: %s: MIDI uses absolute time division which is not supported (v=%d)\n", path, division);
		return NULL;
	}

	struct mid* mid = calloc(1, sizeof *mid);
	mid->division = division;
	arrsetlen(mid->trk_arr, n_tracks);

	for (int i = 0; i < n_tracks; i++) {
		if (!skip_magic_string(f, "MTrk")) {
			fprintf(stderr, "ERROR: %s: expected MTrk (t=%d)\n", path, i);
			return NULL;
		}
		int remaining = read_i32_be(f);
		if (remaining < 0) {
			fprintf(stderr, "ERROR: %s: bad MTrk size\n", path);
			return NULL;
		}
		int pos = 0;
		struct trk* trk = &mid->trk_arr[i];
		int end_of_track = 0;
		int last_b0 = -1;
		int current_midi_channel = -1;
		while (remaining > 0) {
			if (end_of_track) {
				fprintf(stderr, "ERROR: %s: premature end of track marker\n", path);
				return NULL;
			}
			read_counter = 0;
			const int delta = read_midi_varuint(f);
			if (delta < 0) {
				fprintf(stderr, "ERROR: %s: bad timestamp (%d)\n", path, delta);
				return NULL;
			}
			pos += delta;

			int b0 = read_u8(f);
			if (b0 < 0x80) {
				if (last_b0 < 0x80) {
					fprintf(stderr, "ERROR: %s: bad sync? (last_b0=%d) (p=%ld)\n", path, last_b0, ftell(f));
					return NULL;
				}
				b0 = last_b0;
				unread_u8(f);
			}
			last_b0 = b0;
			const int h0 = b0 & 0xf0;
			const int nn = b0 & 0x0f;
			int emit_mev = 0;
			struct mev mev = {
				.pos = pos,
			};
			int nstd = -1;
			if (b0 == 0xf0) { // sysex event
				const int len = read_midi_varuint(f);
				if (len < 0) {
					fprintf(stderr, "ERROR: %s: bad sysex length\n", path);
					return NULL;
				}
				fprintf(stderr, "WARNING: %s: trashing sysex chunk (len=%d)\n", path, len);
				skip_n(f, len-1);
				if (read_u8(f) != 0xf7) {
					fprintf(stderr, "ERROR: %s: bad sysex block\n", path);
					return NULL;
				}
			} else if (b0 == 0xff) { // meta event
				const int type = read_u8(f);
				int write_nmore = -1;
				if (type < 0) {
					fprintf(stderr, "ERROR: %s: bad meta type read\n", path);
					return NULL;
				}
				const int len = read_midi_varuint(f);
				if (len < 0) {
					fprintf(stderr, "ERROR: %s: bad meta type len\n", path);
					return NULL;
				}
				char* str = read_string(f, len);
				unsigned char* data = (unsigned char*)str;
				if (type == TEXT) {
					if (mid->text == NULL) {
						mid->text = str;
						str = NULL; // own str
					} else {
						fprintf(stderr, "WARNING: trashing TEXT [%s]; already got one\n", str);
					}
				} else if (type == TRACK_NAME) {
					if (trk->title == NULL) {
						trk->title = str;
						str = NULL; // own str
					} else {
						fprintf(stderr, "WARNING: trashing TRACK TITLE [%s]; already got one\n", str);
					}
				} else if (type == INSTRUMENT_NAME) {
					fprintf(stderr, "WARNING: trashing INSTRUMENT NAME [%s]\n", str);
				} else if (type == MARKER) {
					fprintf(stderr, "WARNING: trashing MARKER [%s]\n", str);
				} else if (type == MIDI_CHANNEL) {
					if (len != 1) {
						fprintf(stderr, "ERROR: %s: expected len=1 for MIDI_CHANNEL\n", path);
						return NULL;
					}
					write_nmore = 1;
					current_midi_channel = data[0];
				} else if (type == END_OF_TRACK) {
					if (len != 0) {
						fprintf(stderr, "ERROR: %s: expected len=0 for END_OF_TRACK\n", path);
						return NULL;
					}
					end_of_track = 1;
					write_nmore = 0;
				} else if (type == SET_TEMPO) {
					if (len != 3) {
						fprintf(stderr, "ERROR: %s: expected len=3 for SET_TEMPO\n", path);
						return NULL;
					}
					write_nmore = 3;
				} else if (type == SMPTE_OFFSET) {
					if (len != 5) {
						fprintf(stderr, "ERROR: %s: expected len=5 for SMPTE_OFFSET\n", path);
						return NULL;
					}

					fprintf(stderr, "WARNING: trashing SMPTE OFFSET %d:%d:%d %d %d\n", data[0], data[1], data[2], data[3], data[4]);
				} else if (type == TIME_SIGNATURE) {
					if (len != 4) {
						fprintf(stderr, "ERROR: %s: expected len=4 for TIME_SIGNATURE\n", path);
						return NULL;
					}
					write_nmore = 2;
				} else if (type == KEY_SIGNATURE) {
					fprintf(stderr, "WARNING: trashing KEY SIGNATURE\n");
				} else if (type == CUSTOM) {
					// NOTE could put my own stuff here
					fprintf(stderr, "WARNING: trashing sequencer specific meta event\n");
				} else {
					fprintf(stderr, "WARNING: trashing unknown meta event type 0x%.2x\n", type);
				}
				if (str) free(str); // set str to NULL if you take ownership

				if (write_nmore >= 0) {
					assert((type < 0x80) && "conflict with normal MIDI");
					assert(0 <= write_nmore && write_nmore < 4);
					mev.b[0] = type;
					for (int i = 0; i < write_nmore; i++) {
						mev.b[i+1] = data[i];
					}
					emit_mev = 1;
				}
			} else if (h0 == NOTE_OFF) {
				nstd = 2;
			} else if (h0 == NOTE_ON) {
				nstd = 2;
			} else if (h0 == POLY_AFTERTOUCH) {
				nstd = 2;
			} else if (h0 == CONTROL_CHANGE) {
				nstd = 2;
			} else if (h0 == PROGRAM_CHANGE) {
				nstd = 1;
			} else if (h0 == CHANNEL_AFTERTOUCH) {
				nstd = 1;
			} else if (h0 == PITCH_BEND) {
				nstd = 2;
			} else {
				fprintf(stderr, "ERROR: %s: bad sync? (b0=%d) (p=%ld)\n", path, b0, ftell(f));
				return NULL;
			}
			if (nstd >= 0) {
				if (nn != current_midi_channel) {
					if (current_midi_channel == -1) {
						current_midi_channel = nn;
					}
					if (nn != current_midi_channel) {
						fprintf(stderr, "WARNING: channel mismatch nn=%d vs meta=%d\n", nn, current_midi_channel);
					}
				}
				mev.b[0] = b0;
				for (int i = 0; i < nstd; i++) {
					int v = read_u8(f);
					if (v < 0 || v >= 0x80) {
						fprintf(stderr, "ERROR: %s: bad MIDI event read (p=%ld)\n", path, ftell(f));
						return NULL;
					}
					mev.b[i+1] = v;
				}
				emit_mev = 1;
				#if 0
				if (h0 == PROGRAM_CHANGE) {
					printf("PRG %d on channel %d\n", mev.b[1], mev.b[0]&0xf);
				}
				#endif
				if (h0 == CONTROL_CHANGE) {
					const int controller = mev.b[1];
					switch (controller) {
						case VOLUME:
						case PAN:
						case MODULATION_WHEEL:
						case DAMPER_PEDAL:
						case EFFECT1_DEPTH:
						case RESET_ALL_CONTROLLERS:
							// handled (?)
							break;
						default:
							fprintf(stderr, "WARNING: trashing CC[%d]=%d event on channel %d\n", mev.b[1], mev.b[2], mev.b[0]&0xf);
							emit_mev = 0;
							break;
					}
				} else if (h0 == POLY_AFTERTOUCH) {
					fprintf(stderr, "WARNING: trashing POLY_AFTERTOUCH\n");
					emit_mev = 0;
				} else if (h0 == CHANNEL_AFTERTOUCH) {
					fprintf(stderr, "WARNING: trashing CHANNEL_AFTERTOUCH\n");
					emit_mev = 0;
				}
			}
			if (emit_mev) {
				arrput(trk->mev_arr, mev);
			}
			assert(read_counter > 0);
			remaining -= read_counter;
		}
		if (remaining != 0) {
			fprintf(stderr, "ERROR: %s: bad sync? (remaining=%d) (p=%ld)\n", path, remaining, ftell(f));
			return NULL;
		}
		if (!end_of_track) {
			fprintf(stderr, "ERROR: %s: encountered no end of track marker\n", path);
			return NULL;
		}

		#if 0
		printf("trk [%s] nev=%ld MIDI ch %d\n", trk->title, arrlen(trk->mev_arr), current_midi_channel);
		#endif
	}

	return mid;

}

static inline float getsz(float scalar)
{
	return (float)g.cfgsz * scalar;
}


int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <path/to/song.mid>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char* mid_path = argv[1];

	int do_create = 0;
	{
		FILE* f = fopen(mid_path, "rb");
		if (f == NULL) {
			do_create = 1;
		} else {
			fclose(f);
		}
	}
	if (!do_create) {
		g.myd = mid_load(mid_path);
		if (g.myd == NULL) {
			fprintf(stderr, "mid_load(\"%s\") failed\n", mid_path);
			exit(EXIT_FAILURE);
		}
	} else {
		assert(!"TODO create midi file");
	}

	char* MIID_SF2 = getenv("MIID_SF2");
	if (MIID_SF2 == NULL || strlen(MIID_SF2) == 0) {
		fprintf(stderr, "WARNING: disabling audio because MIID_SF2 is not set (should contain colon-separated list of paths to SoundFonts)\n");
	} else {
		g.using_audio = 1;
	}

	char* MIID_SZ = getenv("MIID_SZ");
	if (MIID_SZ != NULL && strlen(MIID_SZ) > 0) {
		g.cfgsz = atoi(MIID_SZ);
		if (g.cfgsz == 0) {
			fprintf(stderr, "WARNING: invalid MIID_SZ value [%s]\n", MIID_SZ);
		}
	} else {
		g.cfgsz = 20;
	}
	const int MIN_CFGSZ = 5;
	if (g.cfgsz < MIN_CFGSZ) g.cfgsz = MIN_CFGSZ;

	assert(SDL_Init(SDL_INIT_TIMER | (g.using_audio?SDL_INIT_AUDIO:0) | SDL_INIT_VIDEO) == 0);
	atexit(SDL_Quit);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

	g.window = SDL_CreateWindow(
		"MiiD",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		1920, 1080,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
	assert(g.window != NULL);

	SDL_GLContext glctx = SDL_GL_CreateContext(g.window);
	assert(glctx);

	g.vg = nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
	assert(g.vg != NULL);

	{
		char* MIID_TTF = getenv("MIID_TTF");
		int font = 0;
		if (MIID_TTF != NULL && strlen(MIID_TTF) > 0) {
			font = nvgCreateFont(g.vg, "font", MIID_TTF);
			if (font == -1) {
				fprintf(stderr, "%s: could not open TTF\n", MIID_TTF);
				exit(EXIT_FAILURE);
			}
		} else {
			font = nvgCreateFontMem(g.vg, "font", font_ttf, font_ttf_len, 0);
			assert((font != -1) && "invalid built-in font");
		}
	}

	populate_screen_globals();

	if (g.using_audio) {
		SDL_AudioSpec have = {0}, want = {0};
		want.freq = 48000;
		want.format = AUDIO_F32;
		want.channels = 2;
		want.samples = 1024;
		want.callback = audio_callback;
		g.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
		assert((g.audio_device > 0) && "failed to open audio device");
		assert(have.channels == want.channels);

		fluid_settings_t* fs = new_fluid_settings();
		assert(fluid_settings_setnum(fs, "synth.sample-rate", have.freq) != FLUID_FAILED);
		assert(fluid_settings_setstr(fs, "synth.midi-bank-select", "gs") != FLUID_FAILED);
		assert(fluid_settings_setint(fs, "synth.polyphony", 256) != FLUID_FAILED);
		assert(fluid_settings_setint(fs, "synth.threadsafe-api", 1) != FLUID_FAILED);
		g.fluid_synth = new_fluid_synth(fs);

		char* cp = copystring(MIID_SF2);
		char* p = cp;
		for (;;) {
			char* p0 = p;
			while (*p && *p != ':') p++;
			const int is_last = (*p == 0);
			*p = 0;
			char* path = copystring(p0);
			arrput(g.soundfont_arr, ((struct soundfont) {
				.path = path,
			}));
			if (is_last) break;
			p++;
		}
		free(cp);

		state.current_soundfont_index = 0;
		refresh_soundfont();

		SDL_PauseAudioDevice(g.audio_device, 0);
	}

	int exiting = 0;
	while (!exiting) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) {
				exiting = 1;
			} else if (e.type == SDL_KEYDOWN) {
				SDL_Keycode sym = e.key.keysym.sym;
				const int ch = 0;
				if (sym == SDLK_ESCAPE) {
					exiting = 1;


				// XXX debugging stuff...
				} else if (sym == '[') {
					state.current_soundfont_index--;
					refresh_soundfont();
				} else if (sym == ']') {
					state.current_soundfont_index++;
					refresh_soundfont();
				} else if (sym == '1') {
					printf("NOTE ON\n");
					fluid_synth_program_change(
						g.fluid_synth,
						ch, 1);
					fluid_synth_noteon(
						g.fluid_synth,
						ch, 60, 127);
				} else if (sym == '2') {
					printf("NOTE OFF\n");
					fluid_synth_noteoff(
						g.fluid_synth,
						ch, 60);
				}
			} else if (e.type == SDL_WINDOWEVENT) {
				if (e.window.event == SDL_WINDOWEVENT_RESIZED) {
					populate_screen_globals();
				}
			}
		}

		glViewport(0, 0, g.true_screen_width, g.true_screen_height);
		glClearColor(0,0,0,1);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

		nvgBeginFrame(g.vg, g.screen_width / g.pixel_ratio, g.screen_height / g.pixel_ratio, g.pixel_ratio);

		#if 0
		nvgBeginPath(g.vg);
		nvgRect(g.vg, 20, 20, 40, 40);
		nvgFillColor(g.vg, nvgRGB(255,255,255));
		nvgFill(g.vg);
		#endif

		{
			nvgFontSize(g.vg, getsz(1));
			nvgTextAlign(g.vg, NVG_ALIGN_LEFT);
			nvgFillColor(g.vg, nvgRGB(255,255,255));

			for (int i = 0; i < arrlen(g.myd->trk_arr); i++) {
				struct trk* trk = &g.myd->trk_arr[i];
				if (trk->title == NULL) continue;
				nvgText(g.vg, getsz(1), (1+i) * getsz(1), trk->title, NULL);
			}
		}

		nvgEndFrame(g.vg);
		SDL_GL_SwapWindow(g.window);
	}

	SDL_GL_DeleteContext(glctx);
	SDL_DestroyWindow(g.window);

	return EXIT_SUCCESS;
}
