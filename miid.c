#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include <SDL.h>
#include "gl.h"

#include <fluidsynth.h>

#include "nanovg.h"
#include "nanovg_gl.h"
//#include "nanovg_gl_utils.h"

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"

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

struct timespan {
	int start;
	int end;
};

struct mev {
	int pos;
	uint8_t b[4];
};

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


static void arr_write_file(uint8_t* out_arr, const char* path)
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

	uint8_t* data = malloc(size);

	if (fread(data, size, 1, f) != 1) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		fclose(f);
		return noblob;
	}

	fclose(f);

	return (struct blob) {
		.data = data,
		.size = size,
	};
}

static inline struct blob blob_slice(struct blob blob, int offset)
{
	if (offset == 0) return blob;
	if (offset > 0) assert((offset <= blob.size) && "slice out of bounds");
	return (struct blob) {
		.data = blob.data + offset,
		.size = blob.size - offset,
	};
}

static inline struct blob arrblob(void* data_arr)
{
	return (struct blob) {
		.data = data_arr,
		.size = arrlen(data_arr),
	};
}

struct medit {
	struct blob mid_blob; // contains valid .mid after edit
	struct timespan affected_timespan;
	int affected_track_index;
	struct timespan selected_timespan;
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
	struct medit* medit_arr;
} g;

struct {
	int current_soundfont_index;
	struct timespan selected_timespan;
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
	if (p->size < n) return 0;
	*p = blob_slice(*p, n);
	return 1;
}

static int skip_magic_string(struct blob* p, char* s)
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
	char* s = malloc(n+1);
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

static struct mid* mid_unmarshal_blob(struct blob blob)
{
	struct blob p = blob;
	if (!skip_magic_string(&p, MThd)) {
		fprintf(stderr, "ERROR: bad header (fourcc)\n");
		return NULL;
	}
	if (read_i32_be(&p) != 6) {
		fprintf(stderr, "ERROR: bad header ("MThd" size not 6)\n");
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

	struct mid* mid = calloc(1, sizeof *mid);
	mid->division = division;
	arrsetlen(mid->trk_arr, n_tracks);

	for (int track_index = 0; track_index < n_tracks; track_index++) {
		const int is_MTrk = skip_magic_string(&p, "MTrk");
		const int chunk_size = read_i32_be(&p);

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
				int write_nmore = -1;
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
					if (mid->text == NULL) {
						mid->text = dup2str(data, len);
					} else {
						fprintf(stderr, "WARNING: trashing TEXT; already got one\n");
					}
				} else if (type == TRACK_NAME) {
					if (trk->name == NULL) {
						trk->name = dup2str(data, len);
					} else {
						fprintf(stderr, "WARNING: trashing TRACK TITLE; already got one\n");
					}
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
					write_nmore = 3;
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
					write_nmore = 2;
				} else if (type == KEY_SIGNATURE) {
					fprintf(stderr, "WARNING: trashing KEY SIGNATURE\n");
				} else if (type == CUSTOM) {
					// NOTE could put my own stuff here
					fprintf(stderr, "WARNING: trashing sequencer specific meta event\n");
				} else {
					fprintf(stderr, "WARNING: trashing unknown meta event type 0x%.2x\n", type);
				}

				if (write_nmore >= 0) {
					assert((type < 0x80) && "conflict with normal MIDI");
					assert(0 <= write_nmore && write_nmore < 4);
					memset(mev.b, 0, ARRAY_LENGTH(mev.b));
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

		#if 0
		printf("trk [%s] nev=%ld MIDI ch %d\n", trk->name, arrlen(trk->mev_arr), current_midi_channel);
		#endif
	}

	if (p.size > 0) {
		fprintf(stderr, "WARNING: ignoring %zd bytes of trailing garbage\n", p.size);
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

		if (trk->name) {
			evbegin(0, &cursor, &bs);
			evmetastr(&bs, TRACK_NAME, trk->name);
		}

		if (trk->midi_channel >= 0) {
			evbegin(0, &cursor, &bs);
			uint8_t* p = evmeta(&bs, MIDI_CHANNEL, 1);
			p[0] = trk->midi_channel;
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
				const int ch = trk->midi_channel;
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
	return (float)g.cfgsz * scalar;
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <path/to/song.mid>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	char* mid_path = argv[1];

	struct blob mid_blob = blob_load(mid_path);
	if (mid_blob.data == NULL) {
		assert(!"TODO create midi file");
	}

	g.myd = mid_unmarshal_blob(mid_blob);
	uint8_t* out_arr = mid_marshal_arr(g.myd);
	arr_write_file(out_arr, "_.mid");

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
				const int ch = 9;
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
				if (trk->name == NULL) continue;
				nvgText(g.vg, getsz(1), (1+i) * getsz(1), trk->name, NULL);
			}
		}

		nvgEndFrame(g.vg);
		SDL_GL_SwapWindow(g.window);
	}

	SDL_GL_DeleteContext(glctx);
	SDL_DestroyWindow(g.window);

	return EXIT_SUCCESS;
}
