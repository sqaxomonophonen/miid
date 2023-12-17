#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include <SDL.h>
#include "gl.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

#include <fluidsynth.h>

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

#include "config.h"

#define ARRAY_LENGTH(xs) (sizeof(xs) / sizeof((xs)[0]))

#define N_NOTES (1<<7)

enum midi_event_type {
	NOTE_OFF             = 0x80, // [key, velocity]
	NOTE_ON              = 0x90, // [key, velocity]
	POLY_AFTERTOUCH      = 0xa0, // [key, pressure]
	CONTROL_CHANGE       = 0xb0, // [controller, value]
	PROGRAM_CHANGE       = 0xc0, // [program]
	CHANNEL_AFTERTOUCH   = 0xd0, // [pressure]
	PITCH_BEND           = 0xe0, // [lsb7, msb7] 14-bit pitch value: lsb7+(msb7<<7)
	SYSEX                = 0xf0,
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

union timespan {
	struct {
		int start;
		int end;
	};
	int s[2];
};

struct mev {
	int pos;
	uint8_t b[4];
};

#define TEXT_FIELD_SIZE (1<<10)

struct trk {
	int midi_channel;
	char* name;
	struct mev* mev_arr;
};

struct mid {
	char* text;
	int division;
	int end_of_song_pos;
	struct trk* trk_arr;
};


static void write_file_from_arr(uint8_t* out_arr, const char* path)
{
	FILE* f = fopen(path, "wb");
	assert((f != NULL) && "cannot write file");
	assert((fwrite(out_arr, arrlen(out_arr), 1, f) == 1) && "cannot write file");
	fclose(f);
}

struct blob {
	uint8_t* data;
	size_t size;
};

static struct blob blob_load(char* path)
{
	struct blob noblob = {0};
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return noblob;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		fclose(f);
		return noblob;
	}

	const long size = ftell(f);

	if (fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		fclose(f);
		return noblob;
	}

	uint8_t* data = (uint8_t*)malloc(size);

	if (fread(data, size, 1, f) != 1) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		fclose(f);
		return noblob;
	}

	fclose(f);

	return (struct blob) {
		.data = data,
		.size = (size_t)size,
	};
}

static inline struct blob blob_slice(struct blob blob, int offset)
{
	if (offset == 0) return blob;
	if (offset > 0) assert((offset <= (int)blob.size) && "slice out of bounds");
	return (struct blob) {
		.data = blob.data + offset,
		.size = blob.size - offset,
	};
}

static inline struct blob arrblob(uint8_t* data_arr)
{
	return (struct blob) {
		.data = data_arr,
		.size = arrlen(data_arr),
	};
}

struct medit {
	struct blob mid_blob; // contains valid .mid after edit
	union timespan affected_timespan;
	int affected_track_index;
	union timespan selected_timespan;
};


const float font_sizes[] = {
	1.0,
	0.75,
};

enum {
	SELECT_BAR = 0,
	SELECT_TICK,
	SELECT_FINE,
};

enum {
	MODE0_EDIT,      // normal edit mode
	MODE0_BLANK,     // no argv; [New Song] [Load Song] [Exit]?
	MODE0_CREATE,    // song foo.mid does not exist; [Create It] [Exit]?
	MODE0_LOAD,      // "load song" dialog
	MODE0_SAVE,      // "save song as" dialog
	MODE0_DO_CLOSE,  // window is closing
};

struct state {
	int mode0;
	SDL_Window* window;
	SDL_GLContext glctx;
	ImGuiContext* imctx;
	char* path;
	struct mid* myd;
	struct medit* medit_arr;
	union timespan selected_timespan;
	float beat0_x;
	float beat_dx;
	bool bar_select;
	int timespan_select_mode = SELECT_BAR;
	bool track_select_set[1<<8];
	int primary_track_select = -1;

	struct {
		bool show_tracks = true;
		int hover_row_index = -1;
		int drag_state;
		float pan_last_x;
		int popup_editing_track_index;
	} header;

	struct {
		int drag_state;
		float pan_last_y;
	} pianoroll;

	float key127_y;
	float key_dy = C_DEFAULT_KEY_HEIGHT_PX;
};


struct g {
	float config_size1;
	int using_audio;
	ImFont* fonts[ARRAY_LENGTH(font_sizes)];
	SDL_AudioDeviceID audio_device;
	fluid_synth_t* fluid_synth;
	int current_soundfont_index;
	struct soundfont* soundfont_arr;
	struct state* state_arr;
	struct state* curstate;
} g;


static inline struct state* curstate(void)
{
	assert(g.curstate != NULL);
	return g.curstate;
}

static void refresh_soundfont(void)
{
	if (!g.using_audio) return;

	const int n = arrlen(g.soundfont_arr);
	if (g.current_soundfont_index >= n) {
		g.current_soundfont_index = 0;
	} else if (g.current_soundfont_index < 0) {
		g.current_soundfont_index = n-1;
	}
	if (g.current_soundfont_index < 0) {
		return;
	}
	struct soundfont* sf = &g.soundfont_arr[g.current_soundfont_index];
	if (sf->error) return;

	const int handle = fluid_synth_sfload(g.fluid_synth, sf->path, /*reset_presets=*/1);
	if (handle == FLUID_FAILED) {
		fprintf(stderr, "WARNING: failed to load SoundFont %s\n", sf->path);
		sf->error = 1;
		return;
	}
	fprintf(stderr, "INFO: loaded SoundFont #%d %s\n",
		g.current_soundfont_index,
		sf->path);
}

// via binfont.c
extern unsigned char font_ttf[];
extern unsigned int font_ttf_len;

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
	char* dst = (char*)malloc(sz+1);
	memcpy(dst, src, sz);
	dst[sz] = 0;
	return dst;
}

static int read_u8(struct blob* p)
{
	if (p->size == 0) return -1;
	const int b = p->data[0];
	*p = blob_slice(*p, 1);
	return b;
}

static void unread_u8(struct blob* p)
{
	*p = blob_slice(*p, -1);
}

static int skip_n(struct blob* p, int n)
{
	if ((int)p->size < n) return 0;
	*p = blob_slice(*p, n);
	return 1;
}

static int skip_magic_string(struct blob* p, const char* s)
{
	const size_t n = strlen(s);
	uint8_t* p0 = p->data;
	int did_skip = skip_n(p, n);
	if (!did_skip) return 0;
	return memcmp(p0, s, n) == 0;
}

static uint8_t* read_data(struct blob* p, int n)
{
	uint8_t* p0 = p->data;
	if (!skip_n(p, n)) return NULL;
	return p0;
}

static char* dup2str(uint8_t* data, int n)
{
	char* s = (char*)malloc(n+1);
	memcpy(s, data, n);
	s[n] = 0;
	return s;
}

static int read_i32_be(struct blob* p)
{
	uint8_t* s = p->data;
	if (!skip_n(p, 4)) return -1;
	const int v =
		(((int)(s[0])) << 24) +
		(((int)(s[1])) << 16) +
		(((int)(s[2])) << 8)  +
		 ((int)(s[3]));
	return v;
}

static int read_u16_be(struct blob* p)
{
	uint8_t* s = p->data;
	if (!skip_n(p, 2)) return -1;
	const int v =
		(((int)(s[0])) << 8)  +
		 ((int)(s[1]));
	return v;
}

static int read_midi_varuint(struct blob* p)
{
	int v = 0;
	for (;;) {
		int ch = read_u8(p);
		if (ch == -1) return -1;
		v |= (ch & 0x7f);
		if (!(ch & 0x80)) break;
		v <<= 7;
	}
	return v;
}

#define MThd "MThd"
#define MTrk "MTrk"

static void handle_text(const char* what, char* text, uint8_t* data, int len)
{
	if (strlen(text) == 0) {
		if (len < (TEXT_FIELD_SIZE-1)) {
			memcpy(text, data, len);
			text[len] = 0;
		} else {
			fprintf(stderr, "WARNING: trashing very long %s\n", what);
		}
	} else {
		fprintf(stderr, "WARNING: trashing %s; already got one\n", what);
	}
}

static char* alloc_text_field(void)
{
	return (char*)calloc(TEXT_FIELD_SIZE, 1);
}

static struct mid* mid_unmarshal_blob(struct blob blob)
{
	struct blob p = blob;
	if (!skip_magic_string(&p, MThd)) {
		fprintf(stderr, "ERROR: bad header (fourcc)\n");
		return NULL;
	}
	if (read_i32_be(&p) != 6) {
		fprintf(stderr, "ERROR: bad header (" MThd " size not 6)\n");
		return NULL;
	}

	const int format = read_u16_be(&p);
	if (format != 1) {
		fprintf(stderr, "ERROR: unsupported format %d; only format 1 is supported, sorry!\n", format);
		return NULL;
	}

	const int n_tracks = read_u16_be(&p);
	if (n_tracks == 0) {
		fprintf(stderr, "ERROR: MIDI file has no tracks; aborting load\n");
		return NULL;
	}

	const int division = read_u16_be(&p);
	if (division >= 0x8000) {
		fprintf(stderr, "ERROR: MIDI uses absolute time division which is not supported (v=%d)\n", division);
		return NULL;
	}

	struct mid* mid = (struct mid*)calloc(1, sizeof *mid);
	mid->division = division;
	mid->text = alloc_text_field();
	arrsetlen(mid->trk_arr, n_tracks);

	const int HAS_META = 1<<0, HAS_MIDI = 1<<1;
	int* trk_flags = NULL;
	arrsetlen(trk_flags, n_tracks);
	int track_index = 0;
	while (track_index < n_tracks) {
		const int is_MTrk = skip_magic_string(&p, "MTrk");
		const int chunk_size = read_i32_be(&p);
		if (chunk_size == -1) {
			fprintf(stderr, "ERROR: bad read");
			return NULL;
		}

		if (!is_MTrk) {
			if (!skip_n(&p, chunk_size)) {
				fprintf(stderr, "ERROR: badly terminated RIFF chunk\n");
				return NULL;
			}
			continue;
		}

		int remaining = chunk_size;
		if (remaining < 0) {
			fprintf(stderr, "ERROR: bad MTrk size\n");
			return NULL;
		}
		int pos = 0;
		struct trk* trk = &mid->trk_arr[track_index];
		memset(trk, 0, sizeof *trk);
		int* flags = &trk_flags[track_index];
		*flags = 0;
		trk->name = alloc_text_field();
		int end_of_track = 0;
		int last_b0 = -1;
		int current_midi_channel = -1;
		while (remaining > 0) {
			if (end_of_track) {
				fprintf(stderr, "ERROR: premature end of track marker\n");
				return NULL;
			}
			struct blob anchor = p;
			const int delta = read_midi_varuint(&p);
			if (delta < 0) {
				fprintf(stderr, "ERROR: bad timestamp (%d)\n", delta);
				return NULL;
			}
			pos += delta;

			int b0 = read_u8(&p);
			if (b0 < 0x80) {
				if (last_b0 < 0x80) {
					fprintf(stderr, "ERROR: bad sync? (last_b0=%d) (p=%ld)\n", last_b0, p.data-blob.data);
					return NULL;
				}
				b0 = last_b0;
				unread_u8(&p);
			}
			last_b0 = b0;
			const int h0 = b0 & 0xf0;
			const int nn = b0 & 0x0f;
			int emit_mev = 0;
			struct mev mev = {
				.pos = pos,
			};
			int nstd = -1;
			if (b0 == SYSEX) { // sysex event
				const int len = read_midi_varuint(&p);
				if (len < 0) {
					fprintf(stderr, "ERROR: bad sysex length\n");
					return NULL;
				}
				fprintf(stderr, "WARNING: trashing sysex chunk (len=%d)\n", len);
				skip_n(&p, len-1);
				if (read_u8(&p) != 0xf7) {
					fprintf(stderr, "ERROR: bad sysex block\n");
					return NULL;
				}
			} else if (b0 == META) { // meta event
				const int type = read_u8(&p);
				int write_nmeta = -1;
				if (type < 0) {
					fprintf(stderr, "ERROR: bad meta type read\n");
					return NULL;
				}
				const int len = read_midi_varuint(&p);
				if (len < 0) {
					fprintf(stderr, "ERROR: bad meta type len\n");
					return NULL;
				}
				uint8_t* data = read_data(&p, len);
				if (data == NULL) {
					fprintf(stderr, "ERROR: bad data\n");
					return NULL;
				}
				if (type == TEXT) {
					handle_text("TEXT", mid->text, data, len);
				} else if (type == TRACK_NAME) {
					handle_text("TRACK TITLE", trk->name, data, len);
				} else if (type == INSTRUMENT_NAME) {
					fprintf(stderr, "WARNING: trashing INSTRUMENT NAME\n");
				} else if (type == MARKER) {
					fprintf(stderr, "WARNING: trashing MARKER\n");
				} else if (type == MIDI_CHANNEL) {
					if (len != 1) {
						fprintf(stderr, "ERROR: expected len=1 for MIDI_CHANNEL\n");
						return NULL;
					}
					if (current_midi_channel == -1) {
						current_midi_channel = data[0];
					} else {
						fprintf(stderr, "WARNING: trashing surplus midi channel meta event\n");
					}
				} else if (type == END_OF_TRACK) {
					if (len != 0) {
						fprintf(stderr, "ERROR: expected len=0 for END_OF_TRACK\n");
						return NULL;
					}
					end_of_track = 1;
					if (pos >= mid->end_of_song_pos) {
						mid->end_of_song_pos = pos;
					}
				} else if (type == SET_TEMPO) {
					if (len != 3) {
						fprintf(stderr, "ERROR: expected len=3 for SET_TEMPO\n");
						return NULL;
					}
					write_nmeta = 3;
				} else if (type == SMPTE_OFFSET) {
					if (len != 5) {
						fprintf(stderr, "ERROR: expected len=5 for SMPTE_OFFSET\n");
						return NULL;
					}

					fprintf(stderr, "WARNING: trashing SMPTE OFFSET %d:%d:%d %d %d\n", data[0], data[1], data[2], data[3], data[4]);
				} else if (type == TIME_SIGNATURE) {
					if (len != 4) {
						fprintf(stderr, "ERROR: expected len=4 for TIME_SIGNATURE\n");
						return NULL;
					}
					write_nmeta = 2;
				} else if (type == KEY_SIGNATURE) {
					fprintf(stderr, "WARNING: trashing KEY SIGNATURE\n");
				} else if (type == CUSTOM) {
					// NOTE could put my own stuff here
					fprintf(stderr, "WARNING: trashing sequencer specific meta event\n");
				} else {
					fprintf(stderr, "WARNING: trashing unknown meta event type 0x%.2x\n", type);
				}

				if (write_nmeta >= 0) {
					assert((type < 0x80) && "conflict with normal MIDI");
					assert(0 <= write_nmeta && write_nmeta < 4);
					memset(mev.b, 0, ARRAY_LENGTH(mev.b));
					mev.b[0] = type;
					for (int i = 0; i < write_nmeta; i++) {
						mev.b[i+1] = data[i];
					}
					*flags |= HAS_META;
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
				fprintf(stderr, "ERROR: bad sync? (b0=%d) (p=%ld)\n", b0, p.data-blob.data);
				return NULL;
			}
			if (nstd >= 0) {
				if (nn != current_midi_channel) {
					if (current_midi_channel == -1) {
						current_midi_channel = nn;
					}
					if (nn != current_midi_channel) {
						fprintf(stderr, "ERROR: channel mismatch nn=%d vs meta=%d\n", nn, current_midi_channel);
						return NULL;
					}
				}
				mev.b[0] = b0;
				for (int i = 0; i < nstd; i++) {
					int v = read_u8(&p);
					if (v < 0 || v >= 0x80) {
						fprintf(stderr, "ERROR: bad MIDI event read (p=%ld)\n", p.data-blob.data);
						return NULL;
					}
					mev.b[i+1] = v;
				}
				*flags |= HAS_MIDI;
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

			const int n_read = p.data - anchor.data;
			assert(n_read > 0);
			remaining -= n_read;
		}
		if (remaining != 0) {
			fprintf(stderr, "ERROR: bad sync? (remaining=%d) (p=%ld)\n", remaining, p.data-blob.data);
			return NULL;
		}
		if (!end_of_track) {
			fprintf(stderr, "ERROR: encountered no end of track marker\n");
			return NULL;
		}

		if (current_midi_channel == -1) {
			// XXX typically seen on first MTrk?
			fprintf(stderr, "WARNING: no MIDI channel (MTrk index %d)\n", track_index);
			trk->midi_channel = -1;
		} else {
			trk->midi_channel = current_midi_channel;
			assert(trk->midi_channel >= 0);
		}

		track_index++;
	}

	if (p.size > 0) {
		fprintf(stderr, "WARNING: ignoring %zd bytes of trailing garbage\n", p.size);
	}

	// we have a constraint that the first track must be a tempo/time
	// signature track, and the rest must be "normal" tracks (and they
	// cannot be mixed). this seems to hold for the MIDI files I've seen,
	// and it makes it easier to use the data as-is. it wouldn't be hard to
	// FIXME but I need a file that breaks this convention.
	for (int i = 0; i < n_tracks; i++) {
		const int flags = trk_flags[i];
		if ((flags & HAS_META) && (flags & HAS_MIDI)) {
			// mixed track
			fprintf(stderr, "ERROR: FIXME fixup not implemented (/1)\n");
			return NULL;
		}
		if (flags & HAS_META) {
			if (i != 0) {
				// meta track is not first
				fprintf(stderr, "ERROR: FIXME fixup not implemented (/2)\n");
				return NULL;
			}
		} else if (flags & HAS_MIDI) {
			if (i == 0) {
				// first track is "normal"
				fprintf(stderr, "ERROR: FIXME fixup not implemented (/3)\n");
				return NULL;
			}
		}
	}

	#if 0
	printf("song length: %d\n", mid->end_of_song_pos);
	#endif

	return mid;
}

static void marshal_raw_string(uint8_t** data_arr, const char* str)
{
	size_t n = strlen(str);
	uint8_t* dst = arraddnptr(*data_arr, n);
	memcpy(dst, str, n);
}

static void store_u32_be(uint8_t* p, unsigned v)
{
	for (int i = 0; i < 4; i++) {
		p[i] = (v >> 24);
		v = (v & 0xffffff) << 8;
	}
}

static void marshal_u32_be(uint8_t** data_arr, unsigned v)
{
	uint8_t* p = arraddnptr(*data_arr, 4);
	store_u32_be(p, v);
}

static void store_u16_be(uint8_t* p, unsigned v)
{
	p[0] = (v >> 8) & 0xff;
	p[1] = v & 0xff;
}

static void marshal_u16_be(uint8_t** data_arr, unsigned v)
{
	uint8_t* p = arraddnptr(*data_arr, 2);
	store_u16_be(p, v);
}

static void marshal_u8(uint8_t** data_arr, unsigned v)
{
	uint8_t* p = arraddnptr(*data_arr, 1);
	*p = v;
}

static void marshal_copy(uint8_t** data_arr, uint8_t* p, int n)
{
	if (n == 0) return;
	uint8_t* d = arraddnptr(*data_arr, n);
	memcpy(d, p, n);
}

static void marshal_midi_varuint(uint8_t** data_arr, unsigned v)
{
	int n_bytes = 1;
	unsigned vc = v;
	for (;;) {
		vc >>= 7;
		if (vc == 0) break;
		n_bytes++;
	}
	uint8_t* p = arraddnptr(*data_arr, n_bytes);
	for (int i = 0; i < n_bytes; i++) {
		p[i] = ((v >> ((n_bytes-1-i)*7)) & 0x7f) | ((i < (n_bytes-1)) ? 0x80 : 0);
	}
}

static void evbegin(int pos, int* cursor, uint8_t** data_arr)
{
	int delta = pos - *cursor;
	assert((delta >= 0) && "bad event ordering");
	marshal_midi_varuint(data_arr, delta);
	*cursor = pos;
}

static uint8_t* evmeta(uint8_t** data_arr, enum meta_type t, int n)
{
	marshal_u8(data_arr, META);
	marshal_u8(data_arr, t);
	marshal_midi_varuint(data_arr, n);
	if (n > 0) {
		uint8_t* p = arraddnptr(*data_arr, n);
		memset(p, 0, n);
		return p;
	} else {
		assert(n == 0);
		return NULL;
	}
}

static void evmetastr(uint8_t** data_arr, enum meta_type t, char* str)
{
	const size_t n = strlen(str);
	uint8_t* p = evmeta(data_arr, t, n);
	memcpy(p, str, n);
}

static int last_marshal_size;
static uint8_t* mid_marshal_arr(struct mid* mid)
{
	uint8_t* bs = NULL;
	if (last_marshal_size > 0) {
		arrsetcap(bs, last_marshal_size);
	}

	const int n_tracks = arrlen(mid->trk_arr);

	marshal_raw_string(&bs, MThd);
	marshal_u32_be(&bs, 6);
	marshal_u16_be(&bs, 1);
	marshal_u16_be(&bs, n_tracks);
	marshal_u16_be(&bs, mid->division);

	for (int track_index = 0; track_index < n_tracks; track_index++) {
		marshal_raw_string(&bs, MTrk);
		const int MTrk_chunk_size_offset = arrlen(bs);
		marshal_u32_be(&bs, -1); // to be written when we know...

		struct trk* trk = &mid->trk_arr[track_index];

		int cursor = 0;

		evbegin(0, &cursor, &bs);
		evmetastr(&bs, TRACK_NAME, trk->name);

		const int midi_channel = trk->midi_channel;

		if (midi_channel >= 0) {
			evbegin(0, &cursor, &bs);
			uint8_t* p = evmeta(&bs, MIDI_CHANNEL, 1);
			p[0] = midi_channel;
		}

		const int n_mev = arrlen(trk->mev_arr);
		int last_midi_cmd = -1;
		for (int mev_index = 0; mev_index < n_mev; mev_index++) {
			struct mev* mev = &trk->mev_arr[mev_index];
			evbegin(mev->pos, &cursor, &bs);
			const uint8_t b0 = mev->b[0];
			int nw = -1;
			int nmeta = -1;
			uint8_t meta[4] = {0};
			if (b0 == SET_TEMPO) {
				for (int i = 0; i < 3; i++) {
					meta[i] = mev->b[i+1];
				}
				nmeta = 3;
			} else if (b0 == TIME_SIGNATURE) {
				for (int i = 0; i < 2; i++) {
					meta[i] = mev->b[i+1];
				}
				meta[2] = 0x24; // XXX?
				meta[3] = 0x08; // XXX?
				nmeta = 4;
			} else {
				switch (b0 & 0xf0) {
				case NOTE_OFF:
				case NOTE_ON:
				case CONTROL_CHANGE:
				case PITCH_BEND:
					nw = 2;
					break;
				case PROGRAM_CHANGE:
					nw = 1;
					break;
				default:
					assert(!"unexpected MIDI event");
				}
			}
			if (nw >= 0) {
				assert(0 < nw && nw <= 3);
				const int ch = midi_channel;
				assert(0 <= ch && ch < 16);
				const int midi_cmd = (b0&0xf0) + ch;
				if (midi_cmd != last_midi_cmd) {
					marshal_u8(&bs, midi_cmd);
					last_midi_cmd = midi_cmd;
				}
				for (int i = 0; i < nw; i++) {
					const int v = mev->b[i+1];
					assert((0 <= v && v < 0x80) && "MIDI argument must not have bit 7 set");
					marshal_u8(&bs, v);
				}
			} else if (nmeta >= 0) {
				const int n0 = arrlen(bs);
				marshal_u8(&bs, META);
				marshal_u8(&bs, b0);
				marshal_u8(&bs, nmeta);
				marshal_copy(&bs, meta, nmeta);
				const int n1 = arrlen(bs);
				assert((n1-n0) == (3+nmeta));
				last_midi_cmd = -1;
			} else {
				assert(!"UNREACHABLE");
			}
		}

		evbegin(mid->end_of_song_pos, &cursor, &bs);
		evmeta(&bs, END_OF_TRACK, 0);

		assert(cursor == mid->end_of_song_pos);

		const int chunk_size = arrlen(bs) - (MTrk_chunk_size_offset + 4);
		store_u32_be(&bs[MTrk_chunk_size_offset], chunk_size);
	}

	last_marshal_size = arrlen(bs);
	printf("mtsz=%d\n", last_marshal_size);
	return bs;
}

static inline float getsz(float scalar)
{
	assert((g.config_size1 > 0) && "called before init?");
	return g.config_size1 * scalar;
}

static inline int dshift(int v, int shift)
{
	if (shift == 0) return v;
	if (shift > 0) return v << shift;
	if (shift < 0) return v >> (-shift);
	assert(!"UNREACHABLE");
}

ImVec4 color_scale(ImVec4 c, float s)
{
	return ImVec4(c.x*s, c.y*s, c.z*s, c.w);
}

ImVec4 color_brighten(ImVec4 c, float s)
{
	return ImVec4(c.x+s, c.y+s, c.z+s, c.w);
}

ImVec4 color_add(ImVec4 a, ImVec4 b)
{
	return ImVec4(a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w);
}

static void track_toggle(int index)
{
	struct state* state = curstate();
	struct mid* mid = state->myd;
	if (mid == NULL) return;
	const int n = arrlen(mid->trk_arr);
	if (!(0 <= index && index < n)) return;
	if (index >= ARRAY_LENGTH(state->track_select_set)) return;
	if (state->track_select_set[index]) {
		state->track_select_set[index] = false;
		if (index == state->primary_track_select) {
			state->primary_track_select = -1;
		}
	} else {
		state->primary_track_select = index;
		state->track_select_set[index] = true;
	}
}

static void ToggleButton(const char* label, bool* p, ImVec4 color)
{
	ImVec4 base = *p ? color : color_scale(color, C_TOGGLE_BUTTON_OFF_SCALAR);
	ImGui::PushStyleColor(ImGuiCol_Button, base);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color_brighten(base, C_TOGGLE_BUTTON_HOVER_BRIGHTEN));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive,  color_brighten(base, C_TOGGLE_BUTTON_ACTIVE_BRIGHTEN));
	if (ImGui::Button(label)) {
		*p = !*p;
	}
	ImGui::PopStyleColor(3);
}

static void g_header(void)
{
	const int IDLE=0, TIMESPAN_PAINT=1, TIME_PAN=2;
	struct state* state = curstate();

	float layout_y0s[1<<8];
	float layout_x1 = 0;
	float layout_w1 = 0;

	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 table_p0 = ImGui::GetCursorScreenPos();
	int new_hover_row_index = -1;
	bool reset_selected_timespan = false;
	int do_popup_editing_track_index = -1;
	bool do_open_op_popup = false;
	bool do_open_song_popup = false;
	const float table_width = ImGui::GetContentRegionAvail().x;
	const int n_columns = 2;
	const ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersV;
	if (ImGui::BeginTable("header", n_columns, table_flags)) {
		ImGui::TableSetupColumn("ctrl",     ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("timeline", ImGuiTableColumnFlags_WidthStretch);

		struct mid* mid = state->myd;
		const int n_tracks = arrlen(mid->trk_arr) - 1;
		struct trk* tracks = &mid->trk_arr[1];
		const int n_rows = 1 + (state->header.show_tracks ? n_tracks : 0);

		bool try_track_select = false;

		for (int row_index = 0; row_index < n_rows; row_index++) {
			const float row_height = row_index == 0 ? getsz(1.5) : getsz(1.0);

			ImGui::PushID(row_index);

			ImGui::TableNextRow();

			assert(row_index < ARRAY_LENGTH(layout_y0s));
			layout_y0s[row_index] = ImGui::GetCursorScreenPos().y;

			if (row_index > 0) {
				ImVec4 c = (row_index & 1) == 1 ? C_TRACK_ROW_EVEN_COLOR : C_TRACK_ROW_ODD_COLOR;
				if (row_index == state->header.hover_row_index) {
					c = color_add(c, C_TRACK_ROW_HOVER_ADD_COLOR);
				}
				const int track_index = row_index - 1;
				if (track_index == state->primary_track_select) {
					c = color_add(c, C_TRACK_ROW_PRIMARY_SELECTION_ADD_COLOR);
				} else if (state->track_select_set[track_index]) {
					c = color_add(c, C_TRACK_ROW_SECONDARY_SELECTION_ADD_COLOR);
				}
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(c));
			}

			ImGui::TableSetColumnIndex(0);
			{
				if (row_index == 0) {
					if (ImGui::Button("Play")) {
					}
					ImGui::SameLine();
					if (ImGui::Button("Loop")) {
					}
					ToggleButton("T", &state->header.show_tracks, C_TRACKS_ROW_TOGGLE_COLOR);
					ImGui::SetItemTooltip("Toggle track rows visibility");

					ImGui::SameLine();

					const ImVec2 bsz = ImVec2(getsz(3),0);
					switch (state->timespan_select_mode) {
					case SELECT_BAR:
						if (ImGui::Button("Bar", bsz)) {
							state->timespan_select_mode = SELECT_TICK;
						}
						break;
					case SELECT_TICK:
						if (ImGui::Button("Tick", bsz)) {
							state->timespan_select_mode = SELECT_FINE;
						}
						break;
					case SELECT_FINE:
						if (ImGui::Button("Fine", bsz)) {
							state->timespan_select_mode = SELECT_BAR;
						}
						break;
					}
					ImGui::SetItemTooltip("Timespan selection mode");

					ImGui::SameLine();
					if (ImGui::Button("Op")) do_open_op_popup = true;
					ImGui::SameLine();
					if (ImGui::Button("Song")) do_open_song_popup = true;
				} else {
					const int track_index = row_index - 1;
					struct trk* trk = &tracks[track_index];
					char label[1<<9];
					snprintf(label, sizeof label, "%s (ch%d)",
						trk->name,
						trk->midi_channel+1);

					const ImVec2 save = ImGui::GetCursorPos();
					ImGui::TextUnformatted(label);
					ImGui::SetCursorPos(save);
					const ImVec2 sz = ImVec2(ImGui::GetColumnWidth(), getsz(1));
					ImGui::InvisibleButton("table_header", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
					if (ImGui::IsItemHovered()) {
						if (ImGui::IsMouseClicked(0)) {
							try_track_select = true;
						} else if (ImGui::IsMouseClicked(1)) {
							do_popup_editing_track_index = track_index;
						}
					}
				}
			}

			ImGui::TableSetColumnIndex(1);
			{
				const float p0x = ImGui::GetCursorScreenPos().x;
				const float cell_width = ImGui::GetContentRegionAvail().x;
				if (p0x > layout_x1) {
					layout_x1 = p0x;
					layout_w1 = cell_width;
				}
				ImGui::Dummy(ImVec2(cell_width, row_height));
			}

			ImGui::PopID();
		}

		ImGui::EndTable();

		assert(n_rows < ARRAY_LENGTH(layout_y0s));
		layout_y0s[n_rows] = ImGui::GetCursorScreenPos().y - 4;

		if (do_open_op_popup) {
			ImGui::OpenPopup("op_popup");
		}
		if (ImGui::BeginPopup("op_popup")) {
			ImGui::SeparatorText("Operations");
			ImGui::TextUnformatted("TODO: set tempo"); // TODO
			ImGui::TextUnformatted("TODO: set time signature"); // TODO
			ImGui::TextUnformatted("TODO: delete range"); // TODO
			ImGui::TextUnformatted("TODO: duplicate range"); // TODO
			ImGui::TextUnformatted("TODO: velocity range operations"); // TODO
			ImGui::TextUnformatted("TODO: delete pitch bend / CC in range"); // TODO
			ImGui::EndPopup();
		}

		if (do_open_song_popup) {
			ImGui::OpenPopup("song_popup");
		}
		if (ImGui::BeginPopup("song_popup")) {
			ImGui::SeparatorText("Song");
			ImGui::InputText("Title", mid->text, TEXT_FIELD_SIZE);
			if (ImGui::InputInt("Division", &mid->division)) {
				if (mid->division < 1) mid->division = 1;
				if (mid->division > 0x7fff) mid->division = 0x7fff;
			}
			if (ImGui::Button("Save")) {
				// TODO
			}
			ImGui::EndPopup();
		}

		if (do_popup_editing_track_index >= 0) {
			state->header.popup_editing_track_index = do_popup_editing_track_index;
			ImGui::OpenPopup("track_edit_popup");
		}
		if (ImGui::BeginPopup("track_edit_popup")) {
			struct trk* trk = &tracks[state->header.popup_editing_track_index];
			ImGui::SeparatorText("Track Edit");

			ImGui::InputText("Name", trk->name, TEXT_FIELD_SIZE);

			if (ImGui::InputInt("MIDI Channel", &trk->midi_channel)) {
				if (trk->midi_channel < 0) trk->midi_channel = 0;
				if (trk->midi_channel > 15) trk->midi_channel = 15;
			}

			const bool can_move_up = state->header.popup_editing_track_index > 0;
			const bool can_move_down = state->header.popup_editing_track_index < n_tracks-1;
			ImGui::BeginDisabled(!can_move_up);
			if (ImGui::Button("Move Up")) {
				struct trk tmp = tracks[state->header.popup_editing_track_index-1];
				tracks[state->header.popup_editing_track_index-1] = tracks[state->header.popup_editing_track_index];
				tracks[state->header.popup_editing_track_index] = tmp;
				state->header.popup_editing_track_index--;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!can_move_down);
			if (ImGui::Button("Down")) {
				struct trk tmp = tracks[state->header.popup_editing_track_index+1];
				tracks[state->header.popup_editing_track_index+1] = tracks[state->header.popup_editing_track_index];
				tracks[state->header.popup_editing_track_index] = tmp;
				state->header.popup_editing_track_index++;
			}
			ImGui::EndDisabled();

			if (state->header.popup_editing_track_index < 10) {
				ImGui::Text("Select with [%c]", "1234567890"[state->header.popup_editing_track_index]);
			}
			ImGui::EndPopup();
		}

		const float mx = io.MousePos.x - layout_x1;
		const float mu = (float)((mx - state->beat0_x) * mid->division) / state->beat_dx;

		{
			ImGui::SetCursorPos(ImVec2(layout_x1, table_p0.y));
			const ImVec2 sz = ImVec2(layout_w1, layout_y0s[n_rows] - layout_y0s[0]);

			ImGui::InvisibleButton("timeline", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // grab mouse wheel

			const bool is_drag = ImGui::IsItemActive();
			const bool is_hover = ImGui::IsItemHovered();

			const bool click_lmb = is_hover && ImGui::IsMouseClicked(0);
			const bool click_rmb = is_hover && ImGui::IsMouseClicked(1);

			if (!is_drag && state->header.drag_state > IDLE) {
				state->header.drag_state = IDLE;
			} else if (click_lmb) {
				const float my = io.MousePos.y;
				if (layout_y0s[0] <= my && my < layout_y0s[1]) {
					state->header.drag_state = TIMESPAN_PAINT;
					reset_selected_timespan = true;
				} else {
					try_track_select = true;
				}
			} else if (click_rmb) {
				state->header.drag_state = TIME_PAN;
				state->header.pan_last_x = 0;
			}

			if (state->header.drag_state == TIME_PAN) {
				const float x = ImGui::GetMouseDragDelta(1).x;
				const float dx = x - state->header.pan_last_x;
				if (dx != 0.0) {
					state->beat0_x += dx;
				}
				state->header.pan_last_x = x;
			}

			if (state->beat_dx == 0.0) state->beat_dx = C_DEFAULT_BEAT_WIDTH_PX;

			const float mw = io.MouseWheel;
			if (is_hover && !is_drag && mw != 0) {
				const float zoom_scalar = powf(C_TIMELINE_ZOOM_SENSITIVITY, mw);
				const float new_beat_dx = state->beat_dx * zoom_scalar;
				state->beat0_x = -((mx - state->beat0_x) / state->beat_dx) * new_beat_dx + mx;
				state->beat_dx = new_beat_dx;
			}
		}

		for (int i = 1; i < n_rows; i++) {
			const float x0 = table_p0.x;
			const float x1 = layout_x1;
			const float x2 = x0 + table_width;
			const float y0 = layout_y0s[i];
			const float y1 = layout_y0s[i+1];
			const ImVec2 p0 = ImVec2(x0, y0);
			const ImVec2 p1 = ImVec2(x1, y1);
			const ImVec2 p2 = ImVec2(x2, y1);
			// HACK: checking mouse cursor shape to prevent
			// selecting track when resizing column width
			if (ImGui::IsMouseHoveringRect(p0, p2) && ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow) {
				new_hover_row_index = i;
				const int track_index = i-1;
				if (try_track_select) {
					track_toggle(track_index);
				}
				break;
			}
		}

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		{
			ImGui::PushFont(g.fonts[1]);
			struct trk* timetrk = &mid->trk_arr[0];
			const int n_timetrack_events = arrlen(timetrk->mev_arr);
			const ImVec2 reserve = ImGui::CalcTextSize("0000.0");
			int numerator = 4;
			int denominator_log2 = 2;
			float beats_per_minute = 120.0;
			int ttpos = 0;
			float bx = state->beat0_x;
			int bar = 0;
			int tick = 0;
			int tickpos = 0;
			int pos = 0;
			int last_spanpos = 0;
			float last_spanbx = bx;

			if (state->header.drag_state == TIMESPAN_PAINT && state->timespan_select_mode == SELECT_FINE) {
				if (reset_selected_timespan) {
					state->selected_timespan.start = mu;
					state->selected_timespan.end = mu;
				} else {
					if (mu < state->selected_timespan.start) {
						state->selected_timespan.start = mu;
						if (state->selected_timespan.start < 0) {
							state->selected_timespan.start = 0;
						}
					}
					if (mu > state->selected_timespan.end) {
						state->selected_timespan.end = mu;
						if (state->selected_timespan.end > mid->end_of_song_pos) {
							state->selected_timespan.start = mid->end_of_song_pos;
						}
					}
				}
			}

			const ImVec2 clip0(layout_x1, table_p0.y);
			const ImVec2 clip1(table_p0.x + table_width, layout_y0s[n_rows]);
			draw_list->PushClipRect(clip0, clip1);

			{
				const float t0 = (float)state->selected_timespan.s[0];
				const float t1 = (float)state->selected_timespan.s[1];
				if (t0 != t1) {
					const float off = layout_x1 + state->beat0_x;
					const float s = state->beat_dx / (float)mid->division;
					const float x0 = off + t0 * s;
					const float x1 = off + t1 * s;
					draw_list->AddRectFilled(
						ImVec2(x0, layout_y0s[0]),
						ImVec2(x1, layout_y0s[1]),
						ImGui::GetColorU32(C_TIMESPAN_TOP_COLOR));
					if (n_rows > 1) {
						draw_list->AddRectFilled(
							ImVec2(x0, layout_y0s[1]),
							ImVec2(x1, layout_y0s[n_rows]),
							ImGui::GetColorU32(C_TIMESPAN_DIM_COLOR));
					}
				}
			}

			while (pos <= mid->end_of_song_pos) {
				const bool is_spanpos =
					(state->header.drag_state == TIMESPAN_PAINT) &&
					(
					((state->timespan_select_mode == SELECT_BAR) && tickpos == 0) ||
					(state->timespan_select_mode == SELECT_TICK)
					);
				if (is_spanpos && last_spanbx <= mx && mx < bx) {
					if (reset_selected_timespan) {
						state->selected_timespan.start = last_spanpos;
						state->selected_timespan.end = pos;
					} else if (state->header.drag_state == TIMESPAN_PAINT) {
						if (last_spanpos < state->selected_timespan.start) {
							state->selected_timespan.start = last_spanpos;
						}
						if (pos > state->selected_timespan.end) {
							state->selected_timespan.end = pos;
						}
					}
				}

				bool has_signature_change = false;
				bool has_tempo_change = false;
				for (;;) {
					if (ttpos >= n_timetrack_events) break;
					struct mev* mev = &timetrk->mev_arr[ttpos];
					if (mev->pos > pos) break;
					const uint8_t b0 = mev->b[0];
					if (b0 == TIME_SIGNATURE) {
						has_signature_change = true;
						numerator = mev->b[1];
						denominator_log2 = mev->b[2];
						if (tickpos != 0) {
							bar++;
							tickpos = 0;
						}
					} else if (b0 == SET_TEMPO) {
						has_tempo_change = true;
						const int microseconds_per_quarter_note =
							((int)(mev->b[1]) << 16) +
							((int)(mev->b[2]) << 8)  +
							((int)(mev->b[3]))       ;
						beats_per_minute = 60e6/microseconds_per_quarter_note;
					} else {
						assert(!"unhandled time track event");
					}
					ttpos++;
				}

				const bool bz = tickpos == 0;

				const float tw = bz ? C_TICK0_WIDTH : C_TICKN_WIDTH;
				const float x0 = layout_x1 + bx - tw*0.5f;
				const float y0 = layout_y0s[0];
				const float x1 = x0+tw;
				const float y1 = layout_y0s[n_rows];

				if (x1 > clip0.x && x0 < clip1.x) {
					draw_list->AddQuadFilled(
						ImVec2(x0,y0),
						ImVec2(x1,y0),
						ImVec2(x1,y1),
						ImVec2(x0,y1),
						ImGui::GetColorU32(bz ? C_TICK0_COLOR : C_TICKN_COLOR));
				}

				const int flog2 = -(denominator_log2-2);
				const float tick_dx = state->beat_dx * powf(2.0, flog2);
				const bool print_per_beat = tick_dx > reserve.x;

				const bool print = bz || print_per_beat;
				char buf[1<<10];

				if (print) {
					if (print_per_beat) {
						if (bz) {
							snprintf(buf, sizeof buf, "%d.%d", bar+1, tickpos+1);
						} else {
							snprintf(buf, sizeof buf, ".%d", tickpos+1);
						}
					} else {
						snprintf(buf, sizeof buf, "%d", bar+1);
					}
					draw_list->AddText(
						ImVec2(x0 + getsz(0.3), layout_y0s[1] - reserve.y),
						ImGui::GetColorU32(bz ? C_BAR_LABEL_COLOR : C_TICK_LABEL_COLOR),
						buf);
				}

				if (has_signature_change || has_tempo_change) {
					const int denominator = 1<<denominator_log2;
					if (has_signature_change && has_tempo_change) {
						snprintf(buf, sizeof buf, "%.1fBPM %d/%d", beats_per_minute, numerator, denominator);
					} else if (has_signature_change) {
						snprintf(buf, sizeof buf, "%d/%d", numerator, denominator);
					} else if (has_tempo_change) {
						snprintf(buf, sizeof buf, "%.1fBPM", beats_per_minute);
					} else {
						assert(!"UNREACHABLE");
					}
					draw_list->AddText(
						ImVec2(x0 + getsz(0.3), y0),
						ImGui::GetColorU32(C_TEMPO_LABEL_COLOR),
						buf);
				}


				if (is_spanpos) {
					last_spanpos = pos;
					last_spanbx = bx;
				}
				pos += dshift(mid->division, flog2);
				tickpos = (tickpos+1) % numerator;
				if (tickpos == 0) bar++;
				bx += tick_dx;
			}

			draw_list->PopClipRect();
			ImGui::PopFont();
		}
	}

	state->header.hover_row_index = new_hover_row_index;
}

static void g_pianoroll(void)
{
	struct state* st = curstate();
	const int IDLE = 0, KEY_PAN = 1;

	const ImVec2 avail = ImGui::GetContentRegionAvail();
	const float table_height = avail.y - 4 - ImGui::GetFrameHeightWithSpacing();
	const int n_columns = 2;
	const ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersV;
	const ImVec2 table_p0 = ImGui::GetCursorScreenPos();
	const ImVec2 table_p1 = ImVec2(table_p0.x + avail.x, table_p0.y + table_height);
	if (ImGui::BeginTable("pianoroll", n_columns, table_flags)) {
		//ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(c));
		ImGui::TableSetupColumn("keys",  ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("notes", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();

		const char* KEYS = "_#_#__#_#_#_";
		assert(strlen(KEYS) == 12);
		//const int C4 = 60;

		ImGuiIO& io = ImGui::GetIO();

		float w0 = 0, w1 = 0;
		ImGui::TableSetColumnIndex(0);
		{
			w0 = ImGui::GetColumnWidth();
			const ImVec2 sz(w0, table_height);
			ImGui::InvisibleButton("keys", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // grab mouse wheel
			const bool is_drag = ImGui::IsItemActive();
			const bool is_hover = ImGui::IsItemHovered();
			const bool click_lmb = is_hover && ImGui::IsMouseClicked(0);
			const bool click_rmb = is_hover && ImGui::IsMouseClicked(1);
			const float mw = io.MouseWheel;
			const float my = io.MousePos.y - table_p0.y;
			if (click_rmb) {
				st->pianoroll.drag_state = KEY_PAN;
				st->pianoroll.pan_last_y = 0;
			}
			if (is_hover && !is_drag && mw != 0) {
				const float zoom_scalar = powf(C_KEY_ZOOM_SENSITIVITY, mw);
				const float new_key_dy = st->key_dy * zoom_scalar;
				st->key127_y = -((my - st->key127_y) / st->key_dy) * new_key_dy + my;
				st->key_dy = new_key_dy;
			}
			if (!is_drag && st->pianoroll.drag_state > IDLE) {
				st->pianoroll.drag_state = IDLE;
			} else if (st->pianoroll.drag_state == KEY_PAN) {
				const float y = ImGui::GetMouseDragDelta(1).y;
				const float dy = y - st->pianoroll.pan_last_y;
				st->key127_y += dy;
				st->pianoroll.pan_last_y = y;
			}
		}

		ImGui::TableSetColumnIndex(1);
		{
			w1 = ImGui::GetColumnWidth();
			const ImVec2 sz(w1, table_height);
			ImGui::InvisibleButton("pianoroll", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // grab mouse wheel
			//const bool is_drag = ImGui::IsItemActive();
			//const bool is_hover = ImGui::IsItemHovered();
		}

		ImGui::EndTable();

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		{
			draw_list->PushClipRect(table_p0, table_p1);

			const float key_size = st->key_dy;

			for (int col = 0; col < 2; col++) {
				float x0=0,x1=0;
				ImU32 black, white;
				if (col == 0) {
					x0 = table_p0.x;
					x1 = x0 + w0;
					black = ImGui::GetColorU32(C_BLACK_KEYS_COLOR);
					white = ImGui::GetColorU32(C_WHITE_KEYS_COLOR);
				} else if (col == 1) {
					const float m = 20;
					x0 = table_p0.x + w0 + m;
					x1 = x0 + w1 - m;
					black = ImGui::GetColorU32(C_BLACK_PIANOROLL_COLOR);
					white = ImGui::GetColorU32(C_WHITE_PIANOROLL_COLOR);
				} else {
					assert(!"UNREACHABLE");
				}

				const float y0 = table_p0.y + st->key127_y;
				draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y0 + 128*key_size), white);

				float y = y0;
				char prev_key = 0;
				for (int note = 127; note >= 0; note--, y += key_size) {
					char key = KEYS[note % 12];
					if (key == '#') {
						const float y0 = y;
						const float y1 = y + key_size;
						draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), black);
					}
					if (key == prev_key) {
						const float y0 = y - 0.5;
						const float y1 = y + 0.5;
						draw_list->AddRectFilled( ImVec2(x0, y0), ImVec2(x1, y1), black);
					}
					prev_key = key;
				}
			}

			draw_list->PopClipRect();
		}
	}

	// toolbar
	if (ImGui::Button("Tool0")) {
	}
	ImGui::SameLine();
	if (ImGui::Button("Tool1")) {
	}
	ImGui::SameLine();
	if (ImGui::Button("Tool2")) {
	}
}

static void g_edit(void)
{
	g_header();
	g_pianoroll();
}

static struct mid* mid_new(void)
{
	struct mid* m = new mid();
	m->text = alloc_text_field();
	strncpy(m->text, "TODO your song title", TEXT_FIELD_SIZE);
	m->division = 480;
	struct trk* trk = arraddnptr(m->trk_arr, 1);
	memset(trk, 0, sizeof *trk);
	trk->midi_channel = -1;
	trk->name = alloc_text_field();
	return m;
}

static void state_new_song(struct state* st)
{
	assert((st->myd == NULL) && "is myd free()'d? why is it not NULL?");
	st->myd = mid_new();
	st->mode0 = MODE0_EDIT;
}

static void g_root(void)
{
	struct state* st = curstate();
	switch (st->mode0) {
	case MODE0_EDIT:
		g_edit();
		break;
	case MODE0_BLANK:
		if (ImGui::Button("Create New Song")) {
			state_new_song(st);
		}
		if (ImGui::Button("Load Song")) {
			st->mode0 = MODE0_LOAD;
		}
		if (ImGui::Button("Exit")) {
			st->mode0 = MODE0_DO_CLOSE;
		}
		break;
	case MODE0_CREATE:
		ImGui::Text("%s: file does not exist", st->path);
		if (ImGui::Button("OK, Create It")) {
			state_new_song(st);
		}
		if (ImGui::Button("Exit")) {
			st->mode0 = MODE0_DO_CLOSE;
		}
		break;
	default:
		ImGui::Text("TODO implement mode0=%d", st->mode0);
		break;
	}
}

static struct state* new_state(void)
{
	struct state* st = arraddnptr(g.state_arr, 1);
	*st = state();
	st->path = alloc_text_field();
	return st;
}

ImFontAtlas* shared_font_atlas = NULL;
static void state_common_init(struct state* st, int mode0)
{
	st->window = SDL_CreateWindow(
		"MiiD",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		1920, 1080,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
	assert(st->window != NULL);

	st->glctx = SDL_GL_CreateContext(st->window);
	assert(st->glctx);

	if (shared_font_atlas == NULL) {
		shared_font_atlas = new ImFontAtlas();
		char* MIID_TTF = getenv("MIID_TTF");
		for (int i = 0; i < ARRAY_LENGTH(font_sizes); i++) {
			const float sz = getsz(font_sizes[i]);
			if (MIID_TTF != NULL && strlen(MIID_TTF) > 0) {
				g.fonts[i] = shared_font_atlas->AddFontFromFileTTF(MIID_TTF, sz);
			} else {
				#ifdef C_TTF
				g.fonts[i] = shared_font_atlas->AddFontFromFileTTF(C_TTF, sz);
				#else
				ImFontConfig cfg;
				cfg.FontDataOwnedByAtlas = false; // memory is static
				g.fonts[i] = shared_font_atlas->AddFontFromMemoryTTF(font_ttf, font_ttf_len, sz, &cfg);
				#endif
			}
		}
		shared_font_atlas->Build();
	}
	assert(shared_font_atlas != NULL);
	st->imctx = ImGui::CreateContext(shared_font_atlas);
	ImGui::SetCurrentContext(st->imctx);
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();
	ImGui_ImplSDL2_InitForOpenGL(st->window, st->glctx);
	ImGui_ImplOpenGL2_Init();

	st->mode0 = mode0;
}

static void push_state_blank(void)
{
	struct state* st = new_state();
	state_common_init(st, MODE0_BLANK);
}

static void push_state_create(const char* path)
{
	struct state* st = new_state();
	strncpy(st->path, path, TEXT_FIELD_SIZE);
	state_common_init(st, MODE0_CREATE);
}

static bool push_state_from_mid_blob(struct blob blob)
{
	struct state* st = new_state();
	st->myd = mid_unmarshal_blob(blob);
	if (st->myd == NULL) {
		return false;
	}
	state_common_init(st, MODE0_EDIT);
	#if 1
	// XXX remove me eventually. currently it's pretty cool though
	uint8_t* out_arr = mid_marshal_arr(st->myd);
	write_file_from_arr(out_arr, "_.mid");
	#endif
	return true;
}

static Uint32 get_event_window_id(SDL_Event* e)
{
	switch (e->type) {
	case SDL_WINDOWEVENT:
		return e->window.windowID;
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		return e->key.windowID;
	case SDL_TEXTEDITING:
		return e->edit.windowID;
	case SDL_TEXTINPUT:
		return e->text.windowID;
	case SDL_MOUSEMOTION:
		return e->motion.windowID;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		return e->button.windowID;
	case SDL_MOUSEWHEEL:
		return e->wheel.windowID;
	case SDL_FINGERMOTION:
	case SDL_FINGERDOWN:
	case SDL_FINGERUP:
		return e->tfinger.windowID;
	case SDL_DROPBEGIN:
	case SDL_DROPFILE:
	case SDL_DROPTEXT:
	case SDL_DROPCOMPLETE:
		return e->drop.windowID;
	default:
		if (SDL_USEREVENT <= e->type && e->type < SDL_LASTEVENT) {
			return e->user.windowID;
		}
		return -1;
	}
	assert(!"UNREACHABLE");
}

int main(int argc, char** argv)
{
	assert(SDL_Init(SDL_INIT_TIMER | (g.using_audio?SDL_INIT_AUDIO:0) | SDL_INIT_VIDEO) == 0);
	atexit(SDL_Quit);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

	char* MIID_SZ = getenv("MIID_SZ");
	if (MIID_SZ != NULL && strlen(MIID_SZ) > 0) {
		g.config_size1 = atoi(MIID_SZ);
		if (g.config_size1 == 0) {
			fprintf(stderr, "WARNING: invalid MIID_SZ value [%s]\n", MIID_SZ);
		}
	} else {
		g.config_size1 = C_DEFAULT_SIZE;
	}

	{
		const float MIN_CONFIG_SIZE1 = 8;
		if (g.config_size1 < MIN_CONFIG_SIZE1) g.config_size1 = MIN_CONFIG_SIZE1;
	}

	IMGUI_CHECKVERSION();

	if (argc == 1) {
		push_state_blank();
	} else {
		for (int ai = 1; ai < argc; ai++) {
			char* mid_path = argv[ai];
			struct blob mid_blob = blob_load(mid_path);
			if (mid_blob.data == NULL) {
				push_state_create(mid_path);
			} else {
				if (!push_state_from_mid_blob(mid_blob)) {
					fprintf(stderr, "ERROR: %s: bad MIDI file\n", mid_path);
					exit(EXIT_FAILURE);
				}
			}
		}
	}

	char* MIID_SF2 = getenv("MIID_SF2");
	if (MIID_SF2 == NULL || strlen(MIID_SF2) == 0) {
		fprintf(stderr, "WARNING: disabling audio because MIID_SF2 is not set (should contain colon-separated list of paths to SoundFonts)\n");
	} else {
		g.using_audio = 1;
	}

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

		g.current_soundfont_index = 0;
		refresh_soundfont();

		SDL_PauseAudioDevice(g.audio_device, 0);
	}

	int exiting = 0;
	while (!exiting) {
		const int n_states = arrlen(g.state_arr);
		if (n_states == 0) break;
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) {
				exiting = 1;
			}
			Uint32 window_id = get_event_window_id(&e);
			for (int i = 0; i < n_states; i++) {
				struct state* st = &g.state_arr[i];
				if (SDL_GetWindowID(st->window) != window_id) continue;
				if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) {
					st->mode0 = MODE0_DO_CLOSE;
				} else {
					ImGui::SetCurrentContext(st->imctx);
					ImGui_ImplSDL2_ProcessEvent(&e);
				}
				break;
			}
		}

		for (int i = 0; i < n_states; i++) {
			struct state* st = g.curstate = &g.state_arr[i];
			#if 1
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
				// XXX remove this eventually?
				st->mode0 = MODE0_DO_CLOSE;
			}
			#endif
			if (st->mode0 == MODE0_DO_CLOSE) {
				ImGui::DestroyContext(st->imctx);
				SDL_GL_DeleteContext(st->glctx);
				SDL_DestroyWindow(st->window);
				// FIXME leaking st->myd, and more?
				arrdel(g.state_arr, i);
				break; // restart main loop
			}

			SDL_GL_MakeCurrent(st->window, st->glctx);
			ImGui::SetCurrentContext(st->imctx);

			ImGuiIO& io = ImGui::GetIO();

			ImGui_ImplOpenGL2_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			ImGui::NewFrame();

			const ImGuiWindowFlags root_window_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground;
			ImGui::SetNextWindowPos(ImVec2(0,0));
			ImGui::SetNextWindowSize(io.DisplaySize);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
			if (ImGui::Begin("root", NULL, root_window_flags)) {
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImGuiStyle().WindowPadding);
				ImGui::PushFont(g.fonts[0]);
				g_root();
				ImGui::PopFont();
				ImGui::PopStyleVar();
				ImGui::End();
			}
			ImGui::PopStyleVar();

			ImGui::Render();
			glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
			SDL_GL_SwapWindow(st->window);
		}
	}

	return EXIT_SUCCESS;
}
