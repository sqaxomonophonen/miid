#include <stdio.h>
#include <assert.h>

#include "stb_ds.h"

#include "config.h"
#include "util.h"

#define RGB2V4(x) ImVec4( \
	(float)(((x)>>16)&0xff) * (1.0f / 255.0f), \
	(float)(((x)>>8)&0xff)  * (1.0f / 255.0f), \
	(float)((x)&0xff)       * (1.0f / 255.0f), \
	1.0f)

#define RGBA2V4(x) ImVec4( \
	(float)(((x)>>24)&0xff) * (1.0f / 255.0f), \
	(float)(((x)>>16)&0xff) * (1.0f / 255.0f), \
	(float)(((x)>>8)&0xff)  * (1.0f / 255.0f), \
	(float)((x)&0xff)       * (1.0f / 255.0f))


#define BOOL(x)     ((struct cval){ .t = T_BOOL        , .b = (x)          })
#define PX(x)       ((struct cval){ .t = T_PX          , .f32 = (x)        })
#define SLIDE(x)    ((struct cval){ .t = T_SLIDE       , .f32 = (x)        })
#define ADD_RGB(x)  ((struct cval){ .t = T_COLOR_ADD   , .v4  = RGB2V4(x)  })
#define ADD_RGBA(x) ((struct cval){ .t = T_COLOR_ADD   , .v4  = RGBA2V4(x) })
#define SUB_RGB(x)  ((struct cval){ .t = T_COLOR_SUB   , .v4  = RGB2V4(x)  })
#define SUB_RGBA(x) ((struct cval){ .t = T_COLOR_SUB   , .v4  = RGBA2V4(x) })
#define MUL_RGB(x)  ((struct cval){ .t = T_COLOR_MUL   , .v4  = RGB2V4(x)  })
#define MUL_RGBA(x) ((struct cval){ .t = T_COLOR_MUL   , .v4  = RGBA2V4(x) })
#define RGB(x)      ((struct cval){ .t = T_COLOR       , .v4  = RGB2V4(x)  })
#define RGBA(x)     ((struct cval){ .t = T_COLOR       , .v4  = RGBA2V4(x) })
#define KEY(x)      ((struct cval){ .t = T_KEY         , .key = (x)        })
#define NONE        ((struct cval){ .t = T_NONE })

static struct cval cvals[] = {
	#define C(NAME,CONSTRUCTOR) CONSTRUCTOR,
	EMIT_CONFIGS
	#undef C
};

static const struct cval DEFAULT_CVALS[] = {
	#define C(NAME,CONSTRUCTOR) CONSTRUCTOR,
	EMIT_CONFIGS
	#undef C
};

#undef BOOL
#undef PX
#undef SLIDE
#undef ADD_RGB
#undef ADD_RGBA
#undef SUB_RGB
#undef SUB_RGBA
#undef MUL_RGB
#undef MUL_RGBA
#undef RGB
#undef RGBA
#undef KEY
#undef NONE

struct cval* config_get_cval(enum config_id id)
{
	assert(0 <= id && id < CONFIG_END);
	return &cvals[id];
}

bool config_get_bool(enum config_id id)
{
	struct cval* v = config_get_cval(id);
	switch (v->t) {
	case T_BOOL:
		return v->b;
	default: assert(!"not bool type");
	}
}

float config_get_float(enum config_id id)
{
	struct cval* v = config_get_cval(id);
	switch (v->t) {
	case T_PX:
	case T_SLIDE:
		return v->f32;
	default: assert(!"not float type");
	}
}

ImVec4 config_get_color(enum config_id id)
{
	struct cval* v = config_get_cval(id);
	switch (v->t) {
	case T_COLOR: return v->v4;
	default: assert(!"not color type");
	}
}

ImVec4 config_color_transform(ImVec4 x, enum config_id id)
{
	struct cval* v = config_get_cval(id);
	ImVec4 y = v->v4;
	switch (v->t) {
	case T_COLOR_ADD: return imvec4_add(x,y);
	case T_COLOR_SUB: return imvec4_sub(x,y);
	case T_COLOR_MUL: return imvec4_mul(x,y);
	default: assert(!"not color transform type");
	}
}

ImGuiKeyChord config_get_key(enum config_id id)
{
	struct cval* v = config_get_cval(id);
	switch (v->t) {
	case T_KEY: return v->key;
	default: assert(!"not key type");
	}
}

static int n_keyjazz_keymaps;
static struct keyjazz_keymap keyjazz_keymaps[1<<8];

struct keyjazz_keymap* get_keyjazz_keymap(int index)
{
	assert(index >= 0);
	if (index >= n_keyjazz_keymaps) return NULL;
	return &keyjazz_keymaps[index];
}

static void add_keyjazz_keymap(const char* s, int offset)
{
	const size_t n = strlen(s);
	for (int i = 0; i < n; i++) {
		assert(n_keyjazz_keymaps < ARRAY_LENGTH(keyjazz_keymaps));
		struct keyjazz_keymap* m = &keyjazz_keymaps[n_keyjazz_keymaps++];
		m->note = offset + i;
		char c = s[i];
		if ('a' <= c && c <= 'z') {
			m->keycode = ImGuiKey(ImGuiKey_A + (c - 'a'));
		} else if ('0' <= c && c <= '9') {
			m->keycode = ImGuiKey(ImGuiKey_0 + (c - '0'));
		} else {
			assert(!"unhandled char");
		}
	}
}

char** sf2_arr;

void config_install(const struct cval* x)
{
	memcpy(cvals, x, sizeof(cvals));
}

void config_set_to_defaults(void)
{
	config_install(DEFAULT_CVALS);
}

void config_init(void)
{
	config_set_to_defaults();

	n_keyjazz_keymaps = 0;
	add_keyjazz_keymap(KEYJAZZ0_KEYMAP_US, 0);
	add_keyjazz_keymap(KEYJAZZ1_KEYMAP_US, 12);

	bool using_audio = false;
	char* MIID_SF2 = getenv("MIID_SF2");
	if (MIID_SF2 == NULL || strlen(MIID_SF2) == 0) {
		fprintf(stderr, "NOTE: disabling audio because MIID_SF2 is not set (should contain colon-separated list of paths to SoundFonts)\n");
	} else {
		using_audio = true;
		char* cp = copystring(MIID_SF2);
		char* p = cp;
		for (;;) {
			char* p0 = p;
			while (*p && *p != ':') p++;
			const int is_last = (*p == 0);
			*p = 0;
			char* path = copystring(p0);
			arrput(sf2_arr, path);
			if (is_last) break;
			p++;
		}
		free(cp);
	}
}

int config_get_soundfont_count(void)
{
	return arrlen(sf2_arr);
}

char* config_get_soundfont_path(int index)
{
	return sf2_arr[index];
}

void config_get_clone(struct cval** x)
{
	const size_t sz = sizeof(cvals);
	if (*x == NULL) *x = (struct cval*)malloc(sz);
	memcpy(*x, cvals, sz);
}

int config_compar(const struct cval* other)
{
	return memcmp(cvals, other, sizeof(cvals));
}

bool config_is_defaults(void)
{
	return config_compar(DEFAULT_CVALS) == 0;
}

static void read_int(FILE* in, int* i)
{
	fscanf(in, "%d", i);
}

static void read_float(FILE* in, float* f)
{
	fscanf(in, "%f", f);
}

static void read_col(FILE* in, ImVec4* c)
{
	fscanf(in, "%f %f %f %f", &c->x, &c->y, &c->z, &c->w);
}

static void read_coltx(FILE* in, struct cval* cv)
{
	ImVec4* c = &cv->v4;
	char buf[1<<10];
	fscanf(in, "%s %f %f %f %f", buf, &c->x, &c->y, &c->z, &c->w);
	if (strcmp(buf, "add") == 0) {
		cv->t = T_COLOR_ADD;
	} else if (strcmp(buf, "sub") == 0) {
		cv->t = T_COLOR_SUB;
	} else if (strcmp(buf, "mul") == 0) {
		cv->t = T_COLOR_MUL;
	} else {
		fprintf(stderr, "WARNING: bad coltx type \"%s\" in config\n", buf);
	}
}

#define CONFIG_FILENAME "miid.conf"

void config_load(void)
{
	FILE* in = fopen(CONFIG_FILENAME, "r");
	if (in == NULL) {
		fprintf(stderr, "skipping %s: no such file (config)\n", CONFIG_FILENAME);
		return;
	}

	#define BOOL(X)     { int i = 0; read_int(in, &i); cv->b = (i>0); }
	#define PX(X)       read_float(in, &cv->f32);
	#define SLIDE(X)    read_float(in, &cv->f32);
	#define KEY(X)      { int i = 0; read_int(in, &i); cv->key = (ImGuiKeyChord)i; }
	#define RGB(X)      read_col(in, &cv->v4);
	#define RGBA(X)     read_col(in, &cv->v4);
	#define ADD_RGB(X)  read_coltx(in, cv);
	#define ADD_RGBA(X) read_coltx(in, cv);
	#define SUB_RGB(X)  read_coltx(in, cv);
	#define SUB_RGBA(X) read_coltx(in, cv);
	#define MUL_RGB(X)  read_coltx(in, cv);
	#define MUL_RGBA(X) read_coltx(in, cv);
	#define NONE

	#define C(NAME,TYPE) \
		if (CN(NAME) < CONFIG_END) { \
			if (strcmp(key, #NAME) == 0) { \
				struct cval* cv = &cvals[CN(NAME)]; \
				TYPE; \
			} \
		}

	//int n = 0;
	for (;;) {
		char key[1<<10];
		int e0 = fscanf(in, "%s", key);
		if (e0 == -1) break;
		EMIT_CONFIGS
		if (fscanf(in, "\n") != 0) break;
		//n++;
	}
	//printf("read %d configs\n", n);

	#undef C

	#undef NONE
	#undef C
	#undef KEY
	#undef RGB
	#undef RGBA
	#undef ADD_RGB
	#undef ADD_RGBA
	#undef SUB_RGB
	#undef SUB_RGBA
	#undef MUL_RGB
	#undef MUL_RGBA
	#undef SLIDE
	#undef PX
	#undef BOOL

	fclose(in);
}

static void write_f32(FILE* out, float f)
{
	fprintf(out, "%f", f);
}

static void write_int(FILE* out, int i)
{
	fprintf(out, "%d", i);
}

static void write_col(FILE* out, ImVec4 v)
{
	fprintf(out, "%f %f %f %f", v.x, v.y, v.z, v.w);
}

static void write_coltx(FILE* out, struct cval* cv)
{
	const char* s;
	switch (cv->t) {
	case T_COLOR_ADD: s = "add"; break;
	case T_COLOR_SUB: s = "sub"; break;
	case T_COLOR_MUL: s = "mul"; break;
	default: assert(!"unhandled type");
	}
	fprintf(out, "%s ", s);
	write_col(out, cv->v4);
}

void config_save(void)
{
	FILE* out = fopen(CONFIG_FILENAME, "w");

	#define BOOL(X)     write_int(out, cv.b ? 1 : 0);
	#define PX(X)       write_f32(out, cv.f32);
	#define SLIDE(X)    write_f32(out, cv.f32);
	#define KEY(X)      write_int(out, (int)cv.key);
	#define RGB(X)      write_col(out, cv.v4);
	#define RGBA(X)     write_col(out, cv.v4);
	#define ADD_RGB(X)  write_coltx(out, &cv);
	#define ADD_RGBA(X) write_coltx(out, &cv);
	#define SUB_RGB(X)  write_coltx(out, &cv);
	#define SUB_RGBA(X) write_coltx(out, &cv);
	#define MUL_RGB(X)  write_coltx(out, &cv);
	#define MUL_RGBA(X) write_coltx(out, &cv);
	#define NONE

	#define C(NAME,TYPE) \
		if (CN(NAME) < CONFIG_END) { \
			fprintf(out, "%s ", #NAME); \
			struct cval cv = cvals[CN(NAME)]; \
			TYPE; \
			fprintf(out, "\n"); \
		}
	EMIT_CONFIGS
	#undef C

	#undef NONE
	#undef C
	#undef KEY
	#undef RGB
	#undef RGBA
	#undef ADD_RGB
	#undef ADD_RGBA
	#undef SUB_RGB
	#undef SUB_RGBA
	#undef MUL_RGB
	#undef MUL_RGBA
	#undef SLIDE
	#undef PX
	#undef BOOL

	fclose(out);
}
