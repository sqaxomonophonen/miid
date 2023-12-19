#include <assert.h>

#include "config.h"
#include "util.h"

enum : int {
	T_NONE,
	T_PX,
	T_SLIDE,
	T_COLOR,
	T_COLOR_ADD,
	T_COLOR_SUB,
	T_COLOR_MUL,
	T_KEY,
};

struct cval {
	int t;
	union {
		float f32;
		ImVec4 v4;
		ImGuiKey key;
	};
};

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


#define PX(x)       ((struct cval){ .t = T_PX          , .f32 = (x)        })
#define SLIDE(x)    ((struct cval){ .t = T_SLIDE       , .f32 = (x)        })
#define ADD_RGB(x)  ((struct cval){ .t = T_COLOR_ADD   , .v4  = RGB2V4(x)  })
#define SUB_RGB(x)  ((struct cval){ .t = T_COLOR_SUB   , .v4  = RGB2V4(x)  })
#define MUL_RGB(x)  ((struct cval){ .t = T_COLOR_MUL   , .v4  = RGB2V4(x)  })
#define RGB(x)      ((struct cval){ .t = T_COLOR       , .v4  = RGB2V4(x)  })
#define RGBA(x)     ((struct cval){ .t = T_COLOR       , .v4  = RGBA2V4(x) })
#define KEY(x)      ((struct cval){ .t = T_KEY         , .key = (x)        })
#define NONE        ((struct cval){ .t = T_NONE })

static struct cval cvals[] = {
	#define C(NAME,CONSTRUCTOR) CONSTRUCTOR,
	EMIT_CONFIGS
	#undef C
};


struct cval* get_cval(enum config_id id)
{
	assert(0 <= id && id < CONFIG_END);
	return &cvals[id];
}

float config_get_float(enum config_id id)
{
	struct cval* v = get_cval(id);
	switch (v->t) {
	case T_PX:
	case T_SLIDE:
		return v->f32;
	default: assert(!"not float type");
	}
}

ImVec4 config_get_color(enum config_id id)
{
	struct cval* v = get_cval(id);
	switch (v->t) {
	case T_COLOR: return v->v4;
	default: assert(!"not color type");
	}
}

ImVec4 config_color_transform(ImVec4 x, enum config_id id)
{
	struct cval* v = get_cval(id);
	ImVec4 y = v->v4;
	switch (v->t) {
	case T_COLOR_ADD: return imvec4_add(x,y);
	case T_COLOR_SUB: return imvec4_sub(x,y);
	case T_COLOR_MUL: return imvec4_mul(x,y);
	default: assert(!"not color transform type");
	}
}

ImGuiKey config_get_key(enum config_id id)
{
	struct cval* v = get_cval(id);
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

void config_init(void)
{
	n_keyjazz_keymaps = 0;
	add_keyjazz_keymap(KEYJAZZ0_KEYMAP_US, 0);
	add_keyjazz_keymap(KEYJAZZ1_KEYMAP_US, 12);
}
