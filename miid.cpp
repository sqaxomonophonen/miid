#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>

#include "imgui.h"
#include "imgui_internal.h"

#include <fluidsynth.h>

#include "stb_ds.h"

#include "generalmidi.h"
#include "config.h"
#include "util.h"
#include "miid.h"

static const char* get_drum_key(int note)
{
	switch (note) {
	#define KEY(N,S) case N: return S;
	EMIT_DRUM_KEYS
	#undef KEY
	default: return NULL;
	}
	assert(!"UNREACHABLE");
}

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
	bool percussive; // true if mev_arr has NOTE ONs, but no NOTE OFFs
};

struct mid {
	char* text;
	int division;
	int end_of_song_pos;
	struct trk* _trk_arr;
};

static inline struct trk* mid_get_time_track(struct mid* mid)
{
	assert(arrlen(mid->_trk_arr) >= 1);
	return &mid->_trk_arr[0];
}

static inline int mid_get_track_count(struct mid* mid)
{
	const int n = arrlen(mid->_trk_arr) - 1;
	assert(n >= 0);
	return n;
}

static inline struct trk* mid_get_trk(struct mid* mid, int index)
{
	const int n = mid_get_track_count(mid);
	assert(0 <= index && index < n);
	return &mid->_trk_arr[1 + index];
}


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

enum {
	SHOW_ALL_TRACKS = 0,
	SHOW_SELECTED_TRACKS,
	SHOW_NO_TRACKS,
};

struct state {
	int mode0;
	char* path;
	struct mid* myd;
	struct medit* medit_arr;
	union timespan selected_timespan;
	float beat0_x;
	float beat_dx;
	bool bar_select;
	int timespan_select_mode = SELECT_BAR;
	bool track_select_set[1<<8];
	bool track_show_set[1<<8];
	int primary_track_select = -1;

	struct {
		int track_show_mode = SHOW_ALL_TRACKS;
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

	bool keyjazz_tester_enabled;
};


struct g {
	bool using_audio;
	ImFont* fonts[ARRAY_LENGTH(font_sizes)];
	fluid_synth_t* fluid_synth;
	int current_soundfont_index;
	bool soundfont_error[1<<10];
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

	const int n = config_get_soundfont_count();
	if (g.current_soundfont_index >= n) {
		g.current_soundfont_index = 0;
	} else if (g.current_soundfont_index < 0) {
		g.current_soundfont_index = n-1;
	}
	const int i = g.current_soundfont_index;
	if (i < 0 || i >= n) {
		return;
	}
	if (i >= ARRAY_LENGTH(g.soundfont_error) || g.soundfont_error[i]) {
		return;
	}
	char* path = config_get_soundfont_path(i);

	const int handle = fluid_synth_sfload(g.fluid_synth, path, /*reset_presets=*/1);
	if (handle == FLUID_FAILED) {
		fprintf(stderr, "WARNING: failed to load SoundFont %s\n", path);
		g.soundfont_error[i] = true;
		return;
	}
	fprintf(stderr, "INFO: loaded SoundFont #%d %s\n",
		i,
		path);
}

// via binfont.c
extern unsigned char font_ttf[];
extern unsigned int font_ttf_len;

void miid_audio_callback(float* stream, int n_frames)
{
	const size_t fsz = 2*sizeof(float);
	memset(stream, 0, fsz * n_frames);
	fluid_synth_write_float(g.fluid_synth,
		n_frames,
		stream, 0, 2,
		stream, 1, 2);
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
	arrsetlen(mid->_trk_arr, n_tracks);

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
		struct trk* trk = &mid->_trk_arr[track_index];
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
				mev.b[0] = b0 & 0xf0; // remove channel (channel is fixed for entire track)
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

		int n_note_on = 0;
		int n_note_off = 0;
		for (struct mev* e = trk->mev_arr; e < trk->mev_arr + arrlen(trk->mev_arr); e++) {
			if (e->b[0] == NOTE_ON && e->b[2] == 0) {
				// XXX is this appropriate? is it a trk
				// "config" like "percussion"? does fluidsynth
				// play it differently?
				e->b[0] = NOTE_OFF;
			}
			switch (e->b[0]) {
			case NOTE_ON:  n_note_on++;  break;
			case NOTE_OFF: n_note_off++; break;
			}
		}
		if (n_note_on > 0 && n_note_off == 0) {
			trk->percussive = true;
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

	const int n_tracks = arrlen(mid->_trk_arr);

	marshal_raw_string(&bs, MThd);
	marshal_u32_be(&bs, 6);
	marshal_u16_be(&bs, 1);
	marshal_u16_be(&bs, n_tracks);
	marshal_u16_be(&bs, mid->division);

	for (int track_index = 0; track_index < n_tracks; track_index++) {
		marshal_raw_string(&bs, MTrk);
		const int MTrk_chunk_size_offset = arrlen(bs);
		marshal_u32_be(&bs, -1); // to be written when we know...

		struct trk* trk = &mid->_trk_arr[track_index];

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
	return CFLOAT(gui_size) * scalar;
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
	const int n = mid_get_track_count(mid);
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

static void MaybeSetItemTooltip(const char* fmt, ...)
{
	if (!CBOOL(show_tooltips)) return;
	va_list args;
	va_start(args, fmt);
	ImGui::SetItemTooltipV(fmt, args);
	va_end(args);
}

static inline float mouse_wheel_scalar(float wheel)
{
	return powf(lerp(1.001f, 1.4f, CFLOAT(wheel_sensitivity)), wheel);
}

struct note_render {
	// setup
	int end_pos;
	float t0;
	float t1;
	float _dt1;
	float clip0x;
	float clip1x;

	// current track
	struct mev* mevs;
	int n_mevs;
	bool percussive;

	// outputs
	float x0,x1;
	uint8_t note, velocity;
};

static void note_render_init(struct note_render* r, int end_pos, float t0, float t1, float clip0x, float clip1x)
{
	memset(r, 0, sizeof *r);
	r->end_pos = end_pos;
	r->t0 = t0;
	r->t1 = t1;
	r->_dt1 = 1.0f / (t1-t0);
	r->clip0x = clip0x;
	r->clip1x = clip1x;
}

static void note_render_do_mevs(struct note_render* r, struct mev* mevs, int n_mevs, bool percussive)
{
	r->mevs = mevs;
	r->n_mevs = n_mevs;
	r->percussive = percussive;
}

static inline bool note_render_next(struct note_render* r)
{
	assert(r->n_mevs >= 0);
	while (r->n_mevs > 0) {
		struct mev* e0 = r->mevs++;
		r->n_mevs--;
		if (e0->b[0] != NOTE_ON) continue;
		r->note = e0->b[1];
		r->velocity = e0->b[2];
		const int p0 = e0->pos;
		r->x0 = lerp(r->clip0x, r->clip1x, (float)((float)p0 - r->t0) * r->_dt1);
		if (!r->percussive) {
			int p1 = r->end_pos;
			// NOTE: mevs/n_mevs have been incremented/decremented at this point
			for (int i = 0; i < r->n_mevs; i++) {
				struct mev* e1 = &r->mevs[i];
				if ((e1->b[0] == NOTE_ON || e1->b[0] == NOTE_OFF) && e1->b[1] == r->note) {
					p1 = e1->pos;
					break;
				}
			}
			r->x1 = lerp(r->clip0x, r->clip1x, (float)(p1 - r->t0) * r->_dt1);
		} else {
			r->x1 = r->x0 + 1;
		}
		return true;

	}
	return false;
}

static int map_row_to_track_index(int row_index)
{
	if (row_index <= 0) return -1;
	struct state* state = curstate();
	struct mid* mid = state->myd;
	const int n_tracks = mid_get_track_count(mid);
	if (state->header.track_show_mode == SHOW_ALL_TRACKS) {
		return row_index - 1;
	} else if (state->header.track_show_mode == SHOW_SELECTED_TRACKS) {
		int cmp = 0;
		for (int i = 0; i < n_tracks; i++) {
			if (state->track_show_set[i]) {
				if (cmp == (row_index-1)) {
					return i;
				}
				cmp++;
			}
		}
		return -1;
	} else {
		return -1;
	}
}

static int must_map_row_to_track_index(int row_index)
{
	int i = map_row_to_track_index(row_index);
	assert(i >= 0);
	return i;
}

static void g_header(void)
{
	const int IDLE=0, TIME_PAINT=1, TIMETRACK_PAINT=2, TIME_PAN=3;
	struct state* state = curstate();

	float layout_y0s[1<<8];
	float layout_x1 = 0;
	float layout_w1 = 0;

	ImGuiIO& io = ImGui::GetIO();
	const ImVec2 table_p0 = ImGui::GetCursorScreenPos();
	int new_hover_row_index = -1;
	bool reset_selection = false;
	int do_popup_editing_track_index = -1;
	bool do_open_op_popup = false;
	bool do_open_song_popup = false;
	const float table_width = ImGui::GetContentRegionAvail().x;
	const int n_columns = 2;
	const ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersV;
	int set_new_track_show_mode = -1;
	if (ImGui::BeginTable("header", n_columns, table_flags)) {
		ImGui::TableSetupColumn("ctrl",     ImGuiTableColumnFlags_WidthFixed);
		ImGui::TableSetupColumn("timeline", ImGuiTableColumnFlags_WidthStretch);

		struct mid* mid = state->myd;
		const int n_tracks = mid_get_track_count(mid);

		int n_selected_tracks = 0;
		int n_show_selected_tracks = 0;
		for (int i = 0; i < n_tracks; i++) {
			if (state->track_select_set[i]) n_selected_tracks++;
			if (state->track_show_set[i]) n_show_selected_tracks++;
		}

		const int n_rows = 1 +
			(
			state->header.track_show_mode == SHOW_ALL_TRACKS       ? n_tracks :
			state->header.track_show_mode == SHOW_SELECTED_TRACKS  ? n_show_selected_tracks :
			state->header.track_show_mode == SHOW_NO_TRACKS        ? 0 :
			-1);
		assert(n_rows > 0);

		for (int row_index = 0; row_index < n_rows; row_index++) {
			const float row_height = row_index == 0 ? getsz(1.5) : getsz(1.0);

			ImGui::PushID(row_index);

			ImGui::TableNextRow();

			assert(row_index < ARRAY_LENGTH(layout_y0s));
			layout_y0s[row_index] = ImGui::GetCursorScreenPos().y;

			int track_index = -1;

			if (row_index > 0) {
				ImVec4 c = (row_index & 1) == 1 ? CCOL(track_row_even_color) : CCOL(track_row_odd_color);
				if (row_index == state->header.hover_row_index) {
					c = CCOLTX(c, track_row_hover_coltx);
				}
				track_index = must_map_row_to_track_index(row_index);
				if (track_index == state->primary_track_select) {
					c = CCOLTX(c, primary_track_coltx);
				} else if (state->track_select_set[track_index]) {
					c = CCOLTX(c, other_track_coltx);
				}
				ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(c));
			}

			if (row_index > 0) assert(track_index >= 0);

			ImGui::TableSetColumnIndex(0);
			{
				if (row_index == 0) {
					if (ImGui::Button("Play")) {
					}
					ImGui::SameLine();
					if (ImGui::Button("Loop")) {
					}

					if (state->header.track_show_mode == SHOW_ALL_TRACKS) {
						if (ImGui::Button("A")) {
							if (n_selected_tracks > 0) {
								for (int i = 0; i < n_tracks; i++) {
									state->track_show_set[i] = state->track_select_set[i];
								}
								set_new_track_show_mode = SHOW_SELECTED_TRACKS;
							} else {
								set_new_track_show_mode = SHOW_NO_TRACKS;
							}
						}
						MaybeSetItemTooltip("Showing all tracks");
					} else if (state->header.track_show_mode == SHOW_SELECTED_TRACKS) {
						if (ImGui::Button("S")) {
							set_new_track_show_mode = SHOW_NO_TRACKS;
						}
						MaybeSetItemTooltip("Showing selected tracks");
					} else if (state->header.track_show_mode == SHOW_NO_TRACKS) {
						if (ImGui::Button("N")) {
							set_new_track_show_mode = SHOW_ALL_TRACKS;
						}
						MaybeSetItemTooltip("Showing no tracks");
					} else {
						assert(!"UNREACHABLE");
					}

					MaybeSetItemTooltip("Toggle track rows visibility");

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
					MaybeSetItemTooltip("Timespan selection mode");

					ImGui::SameLine();
					if (ImGui::Button("Op")) do_open_op_popup = true;
					MaybeSetItemTooltip("Range edits");
					ImGui::SameLine();
					if (ImGui::Button("Song")) do_open_song_popup = true;
					MaybeSetItemTooltip("Edit song");
				} else {
					struct trk* trk = mid_get_trk(mid, track_index);
					char label[1<<9];
					snprintf(label, sizeof label, "%s (ch%d)##%d",
						trk->name,
						trk->midi_channel+1,
						track_index);
					const ImVec2 sz = ImVec2(ImGui::GetColumnWidth(), 0);
					if (ImGui::ButtonEx(label, sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight)) {
						if (ImGui::IsMouseReleased(0)) {
							track_toggle(track_index);
						} else if (ImGui::IsMouseReleased(1)) {
							do_popup_editing_track_index = track_index;
						}
					}
					MaybeSetItemTooltip("Left-click: toggle. Right-click: edit");
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
			ImGui::TextUnformatted("TODO: time crop (set start/end/both)"); // TODO
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
			struct trk* trk = mid_get_trk(mid, state->header.popup_editing_track_index);
			ImGui::SeparatorText("Track Edit");

			ImGui::InputText("Name", trk->name, TEXT_FIELD_SIZE);

			int human_midi_channel = trk->midi_channel + 1;
			if (ImGui::InputInt("MIDI Channel", &human_midi_channel)) {
				if (human_midi_channel < 1)  human_midi_channel = 1;
				if (human_midi_channel > 16) human_midi_channel = 16;
				trk->midi_channel = human_midi_channel - 1;
			}

			if (ImGui::Checkbox("Percussive (NOTE ON only)", &trk->percussive)) {
				if (trk->percussive) {
					printf("TODO remove NOTE OFF events\n"); // TODO
				} else {
					// TODO maybe insert NOTE OFFs at end
					// of song? or maybe have notes time
					// out after 1 beat or something?
				}
			}

			const bool can_move_up = state->header.popup_editing_track_index > 0;
			const bool can_move_down = state->header.popup_editing_track_index < n_tracks-1;
			ImGui::BeginDisabled(!can_move_up);
			struct trk* _tracks = mid_get_trk(mid, 0);
			if (ImGui::Button("Move Up")) {
				struct trk tmp = _tracks[state->header.popup_editing_track_index-1];
				_tracks[state->header.popup_editing_track_index-1] = _tracks[state->header.popup_editing_track_index];
				_tracks[state->header.popup_editing_track_index] = tmp;
				state->header.popup_editing_track_index--;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!can_move_down);
			if (ImGui::Button("Down")) {
				struct trk tmp = _tracks[state->header.popup_editing_track_index+1];
				_tracks[state->header.popup_editing_track_index+1] = _tracks[state->header.popup_editing_track_index];
				_tracks[state->header.popup_editing_track_index] = tmp;
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
			const bool doubleclick_rmb = is_hover && ImGui::IsMouseDoubleClicked(1);
			const float t0 = (float)state->selected_timespan.start / (float)mid->division;
			const float t1 = (float)state->selected_timespan.end   / (float)mid->division;
			if (doubleclick_rmb && t1 > t0) {
				const float x0 = layout_x1;
				const float x1 = x0 + sz.x;
				const float s = lerp(0.0f, 0.49f, CFLOAT(time_fit_padding));
				const float vx0 = lerp(x0, x1, s);
				const float vx1 = lerp(x0, x1, 1.0f-s);
				state->beat_dx = (vx1-vx0) / (t1-t0);
				state->beat0_x = -t0 * state->beat_dx + (vx0 - x0);
			}

			if (!is_drag && state->header.drag_state > IDLE) {
				state->header.drag_state = IDLE;
			} else if (click_lmb) {
				const float my = io.MousePos.y;
				reset_selection = !io.KeyShift;
				if (layout_y0s[0] <= my && my < layout_y0s[1]) {
					state->header.drag_state = TIME_PAINT;
				} else {
					state->header.drag_state = TIMETRACK_PAINT;
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
				const float zoom_scalar = mouse_wheel_scalar(mw);
				const float new_beat_dx = state->beat_dx * zoom_scalar;
				state->beat0_x = -((mx - state->beat0_x) / state->beat_dx) * new_beat_dx + mx;
				state->beat_dx = new_beat_dx;
			}
		}

		for (int i = 1; i < n_rows; i++) {
			const float x0 = table_p0.x;
			const float x1 = x0 + table_width;
			const float y0 = layout_y0s[i];
			const float y1 = layout_y0s[i+1];
			const ImVec2 p0 = ImVec2(x0, y0);
			const ImVec2 p1 = ImVec2(x1, y1);
			if (ImGui::IsMouseHoveringRect(p0, p1) && ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow) {
				new_hover_row_index = i;
				break;
			}
		}

		ImDrawList* draw_list = ImGui::GetWindowDrawList();

		{
			ImGui::PushFont(g.fonts[1]);
			struct trk* timetrk = mid_get_time_track(mid);
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
			int last_spanpos = 0;
			float last_spanbx = bx;

			const bool is_track_painting = state->header.drag_state == TIMETRACK_PAINT;
			const bool is_time_painting = (state->header.drag_state == TIME_PAINT) || (state->header.drag_state == TIMETRACK_PAINT);

			if (is_track_painting && new_hover_row_index >= 1) {
				if (reset_selection) {
					for (int i = 0; i < n_tracks; i++) {
						state->track_select_set[i] = false;
					}
				}
				const int track_index = map_row_to_track_index(new_hover_row_index);
				if (track_index >= 0) {
					state->primary_track_select = track_index;
					state->track_select_set[track_index] = true;
				}
			}

			if (is_time_painting && state->timespan_select_mode == SELECT_FINE) {
				if (reset_selection) {
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
						CCOL32(timespan_top_color));
					if (n_rows > 1) {
						draw_list->AddRectFilled(
							ImVec2(x0, layout_y0s[1]),
							ImVec2(x1, layout_y0s[n_rows]),
							CCOL32(timespan_dim_color));
					}
				}
			}

			int pos = 0;
			while (pos <= mid->end_of_song_pos) {
				const bool is_spanpos =
					is_time_painting &&
					(
					((state->timespan_select_mode == SELECT_BAR) && tickpos == 0) ||
					(state->timespan_select_mode == SELECT_TICK)
					);
				if (is_spanpos && last_spanbx <= mx && mx < bx) {
					if (reset_selection) {
						state->selected_timespan.start = last_spanpos;
						state->selected_timespan.end = pos;
					} else {
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

				const float tw = bz ? CFLOAT(tick0_size) : CFLOAT(tickn_size);
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
						bz ? CCOL32(tick0_color) : CCOL32(tickn_color));
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
						bz ? CCOL32(bar_label_color) : CCOL32(tick_label_color),
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
						CCOL32(tempo_label_color),
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

			struct note_render nr;
			const float t0 = (-state->beat0_x / state->beat_dx) * (float)mid->division;
			const float t1 = t0 + ((clip1.x - clip0.x) / state->beat_dx) * (float)mid->division;
			note_render_init(&nr, mid->end_of_song_pos, t0, t1, clip0.x, clip1.x);
			const int nmod = 12; // FIXME?
			const ImVec4 c0 = CCOL(pianoroll_note_color0);
			const ImVec4 c1 = CCOL(pianoroll_note_color1);
			for (int i0 = 1; i0 < n_rows; i0++) {
				const float y0 = layout_y0s[i0];
				const float y1 = layout_y0s[i0+1];
				struct trk* trk = mid_get_trk(mid, must_map_row_to_track_index(i0));
				const bool percussive = trk->percussive;
				note_render_do_mevs(&nr, trk->mev_arr, arrlen(trk->mev_arr), percussive);
				while (note_render_next(&nr)) {
					const float x0 = nr.x0;
					const float x1 = nr.x1;
					const int note = (int)nr.note % nmod;
					const float y = y0 + (nmod - 1 - note) + 0.5f*((y1-y0)-getsz(1)); // XXX scale?
					ImVec4 cc = imvec4_lerp(c0, c1, (float)nr.velocity / 127.0f);
					const ImU32 color = ImGui::GetColorU32(cc);
					if (percussive) {
						draw_list->AddCircleFilled(ImVec2(x0,y), 2, color);
					} else {
						draw_list->AddRectFilled(
							ImVec2(x0, y-0.5),
							ImVec2(x1, y+0.5),
							color);
					}
				}
			}

			draw_list->PopClipRect();
			ImGui::PopFont();
		}
	}

	state->header.hover_row_index = new_hover_row_index;

	if (set_new_track_show_mode >= 0) {
		state->header.track_show_mode = set_new_track_show_mode;
	}

}

static void g_pianoroll(void)
{
	struct state* st = curstate();
	const int IDLE = 0, KEY_PAN = 1;

	bool is_drum_track = false;

	struct mid* mid = st->myd;
	if (st->primary_track_select >= 0) {
		struct trk* trk = mid_get_trk(mid, st->primary_track_select);
		if (trk->midi_channel == 9) {
			is_drum_track = true;
		}
	}

	const union timespan selected_timespan = st->selected_timespan;
	const bool have_selected_timespan = selected_timespan.end > selected_timespan.start;

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

		const char* KEYS = "C#D#EF#G#A#B";
		assert(strlen(KEYS) == 12);
		//const int C4 = 60;

		ImGuiIO& io = ImGui::GetIO();

		bool try_note_fit = false;

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
			const bool doubleclick_rmb = is_hover && ImGui::IsMouseDoubleClicked(1);
			if (doubleclick_rmb) try_note_fit = true;
			const float mw = io.MouseWheel;
			const float my = io.MousePos.y - table_p0.y;
			if (click_rmb) {
				st->pianoroll.drag_state = KEY_PAN;
				st->pianoroll.pan_last_y = 0;
			}
			if (is_hover && !is_drag && mw != 0) {
				const float zoom_scalar = mouse_wheel_scalar(mw);
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

		ImGui::PushFont(g.fonts[1]);

		const float line_height = ImGui::GetTextLineHeight();
		const bool print_key_labels = st->key_dy > line_height;
		const float key_size = st->key_dy;

		const float table_separator = 20;

		{
			draw_list->PushClipRect(table_p0, table_p1);

			const ImU32 key_label_color = CCOL32(key_label_color);

			for (int col = 0; col < 2; col++) {
				float x0=0,x1=0;
				ImU32 black, white;
				if (col == 0) {
					x0 = table_p0.x;
					x1 = x0 + w0;
					black = CCOL32(black_keys_color);
					white = CCOL32(white_keys_color);
				} else if (col == 1) {
					x0 = table_p0.x + w0 + table_separator;
					x1 = x0 + w1 - table_separator;
					black = CCOL32(black_pianoroll_color);
					white = CCOL32(white_pianoroll_color);
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
					if ((key == '#') == (prev_key == '#')) {
						const float y0 = y - 0.5;
						const float y1 = y + 0.5;
						draw_list->AddRectFilled( ImVec2(x0, y0), ImVec2(x1, y1), black);
					}
					if (col == 0 && print_key_labels) {
						char buf[1<<10];
						const char* key_label = NULL;
						if (is_drum_track) {
							key_label = get_drum_key(note);
						}
						if (key_label == NULL) {
							const char* sep = "";
							char k = key;
							if (k == '#') {
								sep = "#";
								assert(note > 0);
								k = KEYS[(note+11) % 12];
							}
							const int octave = (note/12)-1;
							snprintf(buf, sizeof buf, "%c%s%d", k, sep, octave);
							key_label = buf;
						}
						draw_list->AddText(ImVec2(x0 + getsz(0.3), y + key_size/2 - line_height/2), key_label_color, key_label);
					}
					prev_key = key;
				}
			}

			draw_list->PopClipRect();
		}

		if (have_selected_timespan) {
			const ImVec2 clip0(table_p0.x + w0 + table_separator, table_p0.y);
			const ImVec2 clip1(clip0.x + w1 - table_separator,    table_p1.y);
			draw_list->PushClipRect(clip0, clip1);

			const int t0 = selected_timespan.start;
			const int t1 = selected_timespan.end;
			const float dt = (float)(t1-t0);

			const ImVec4 c0 = CCOL(pianoroll_note_color0);
			const ImVec4 c1 = CCOL(pianoroll_note_color1);
			const ImU32 border_color = CCOL32(pianoroll_note_border_color);
			const float border_size = CFLOAT(pianoroll_note_border_size);

			struct note_render nr;
			note_render_init(&nr, mid->end_of_song_pos, t0, t1, clip0.x, clip1.x);

			int note_min = -1;
			int note_max = -1;
			const int n_tracks = mid_get_track_count(mid);
			for (int pass = 0; pass < 2; pass++) {
				if (pass == 1 && st->primary_track_select < 0) break;
				for (int track_index = 0; track_index < n_tracks; track_index++) {
					if (pass == 0 && !st->track_select_set[track_index]) continue;
					if (pass == 0 && track_index == st->primary_track_select) continue;
					if (pass == 1 && track_index != st->primary_track_select) continue;
					//const bool is_primary = (pass == 1);
					const bool is_other = (pass == 0);

					struct trk* trk = mid_get_trk(mid, track_index);
					const bool percussive = trk->percussive;
					note_render_do_mevs(&nr, trk->mev_arr, arrlen(trk->mev_arr), percussive);

					while (note_render_next(&nr)) {
						const float x0 = nr.x0;
						const float x1 = nr.x1;
						const int note = nr.note;
						const float y0 = clip0.y + st->key127_y + (float)(127-note) * st->key_dy;
						const float y1 = y0 + st->key_dy;
						ImVec4 cc = imvec4_lerp(c0, c1, (float)nr.velocity / 127.0f);
						if (is_other) cc = CCOLTX(cc, pianoroll_note_other_track_coltx);
						const ImU32 color = ImGui::GetColorU32(cc);
						bool is_visible = false;
						if (!percussive) {
							if (x1 > clip0.x && x0 < clip1.x) {
								draw_list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), color);
								if (border_size > 0 && border_color > 0) {
									draw_list->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), border_color, 0, 0, border_size);
								}
								is_visible = true;
							}
						} else {
							if (x0 >= clip0.x && x0 <= clip1.x) {
								const float m = CFLOAT(percussion_line_width);
								if (m > 0) {
									draw_list->AddRectFilled(ImVec2(x0-m, y0), ImVec2(x0+m, y1), color);
									is_visible = true;
								}
								const float r = CFLOAT(percussion_dot_radius);
								if (r > 0) {
									draw_list->AddCircleFilled(ImVec2(x0, (y0+y1)*0.5f), r, color);
									is_visible = true;
								}
							}
						}
						if (is_visible) {
							if (note_min == -1) {
								note_min = note;
								note_max = note;
							}  else {
								if (note < note_min) note_min = note;
								if (note > note_max) note_max = note;
							}
						}
					}
				}
			}

			draw_list->PopClipRect();

			if (try_note_fit && note_min != -1) {
				const float s = lerp(0.0f, 0.4f, CFLOAT(note_fit_padding));
				const float vy0 = lerp(clip0.y, clip1.y, s);
				const float vy1 = lerp(clip0.y, clip1.y, 1-s);
				assert(note_max >= note_min);
				st->key_dy = (float)(vy1 - vy0) / (float)(note_max - note_min + 1);
				st->key127_y = (float)(note_max - 127) * st->key_dy + (vy0 - clip0.y);
			}
		}

		ImGui::PopFont();
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
	struct state* st = curstate();

	assert((st->myd != NULL) && "must have myd at this point");

	if (ImGui::IsWindowFocused()) {
		if (ImGui::IsKeyPressed(CKEY(toggle_keyjazz_tester_key))) {
			st->keyjazz_tester_enabled = !st->keyjazz_tester_enabled;
			if (!st->keyjazz_tester_enabled) {
				printf("TODO kill notes\n"); // TODO
			}
		}
		if (st->keyjazz_tester_enabled) {
			int i = 0;
			for (;;) {
				struct keyjazz_keymap* m = get_keyjazz_keymap(i++);
				if (m == NULL) break;
				const int ch = 0; // XXX
				const int key = 48 + m->note; // XXX
				const int vel = 100; // XXX
				if (ImGui::IsKeyPressed(m->keycode, false)) {
					fluid_synth_noteon(g.fluid_synth, ch,  key, vel);
				}
				if (ImGui::IsKeyReleased(m->keycode)) {
					fluid_synth_noteoff(g.fluid_synth, ch,  key);
				}
			}
		}
	} else {
	}

	g_header();
	g_pianoroll();
}

static struct mid* mid_new(void)
{
	struct mid* m = new mid();
	m->text = alloc_text_field();
	strncpy(m->text, "TODO your song title", TEXT_FIELD_SIZE);
	m->division = 480;
	struct trk* trk = arraddnptr(m->_trk_arr, 1);
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
	struct state* st = new state();
	st->path = alloc_text_field();
	return st;
}

ImFontAtlas* shared_font_atlas = NULL;
static void state_common_init(struct state* st, int mode0)
{
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
	miidhost_create_window(st, shared_font_atlas);

	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

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

void miid_init(int argc, char** argv, float sample_rate)
{
	g.using_audio = sample_rate > 0;

	fluid_settings_t* fs = new_fluid_settings();
	assert(fluid_settings_setnum(fs, "synth.sample-rate", sample_rate) != FLUID_FAILED);
	assert(fluid_settings_setstr(fs, "synth.midi-bank-select", "gs") != FLUID_FAILED);
	assert(fluid_settings_setint(fs, "synth.polyphony", 256) != FLUID_FAILED);
	assert(fluid_settings_setint(fs, "synth.threadsafe-api", 1) != FLUID_FAILED);
	g.fluid_synth = new_fluid_synth(fs);

	g.current_soundfont_index = 0;
	refresh_soundfont();

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

}

bool miid_frame(void* usr, bool request_close)
{
	struct state* st = (struct state*)usr;
	g.curstate = st;

	ImGuiIO& io = ImGui::GetIO();
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

	if (request_close) st->mode0 = MODE0_DO_CLOSE; // TODO?

	return st->mode0 == MODE0_DO_CLOSE;
}
