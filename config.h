#ifndef CONFIG_H

#include "imgui.h"

//  key                                   type/default
#define EMIT_CONFIGS                                                      \
C(  gui_size                            , PX(20)                        ) \
C(  wheel_sensitivity                   , SLIDE(0.2)                    ) \
C(  toggle_button_off_coltx             , MUL_RGB(0x444444)             ) \
C(  toggle_button_hover_coltx           , ADD_RGB(0x303030)             ) \
C(  toggle_button_active_coltx          , ADD_RGB(0x808080)             ) \
C(  tracks_toggle_color                 , RGB(0x008020)                 ) \
C(  track_row_even_color                , RGB(0x111111)                 ) \
C(  track_row_odd_color                 , RGB(0x000011)                 ) \
C(  track_row_hover_coltx               , ADD_RGB(0x111111)             ) \
C(  primary_track_coltx                 , ADD_RGB(0x554400)             ) \
C(  other_track_coltx                   , ADD_RGB(0x001166)             ) \
C(  tick0_size                          , PX(2.0)                       ) \
C(  tick0_color                         , RGBA(0xffffff20)              ) \
C(  tickn_size                          , PX(1.0)                       ) \
C(  tickn_color                         , RGBA(0xffffff10)              ) \
C(  bar_label_color                     , RGB(0xffff00)                 ) \
C(  tick_label_color                    , RGB(0x555500)                 ) \
C(  tempo_label_color                   , RGB(0xcc77cc)                 ) \
C(  key_label_color                     , RGB(0x000000)                 ) \
C(  timespan_top_color                  , RGB(0x665500)                 ) \
C(  timespan_dim_color                  , RGBA(0x66550050)              ) \
C(  track_data_color                    , RGBA(0x80ff0030)              ) \
C(  white_keys_color                    , RGB(0x444444)                 ) \
C(  black_keys_color                    , RGB(0x333333)                 ) \
C(  white_pianoroll_color               , RGB(0x161616)                 ) \
C(  black_pianoroll_color               , RGB(0x101010)                 ) \
C(  pianoroll_note_color0               , RGBA(0xff000030)              ) \
C(  pianoroll_note_color1               , RGBA(0xff80ffff)              ) \
C(  pianoroll_note_border_color         , RGBA(0x00000077)              ) \
C(  pianoroll_note_border_size          , PX(1)                         ) \
C(  pianoroll_note_other_track_coltx    , MUL_RGBA(0x4080ff80)          ) \
C(  toggle_keyjazz_tester_key           , KEY(ImGuiKey_GraveAccent)     ) \
C(  END                                 , NONE                          )

#define CN(NAME) CONFIG_ ## NAME

enum config_id {
	#define C(NAME,DEFAULT) CN(NAME),
	EMIT_CONFIGS
	#undef C
};

float  config_get_float(enum config_id);
ImVec4 config_get_color(enum config_id);
ImVec4 config_color_transform(ImVec4 x, enum config_id);
ImGuiKey config_get_key(enum config_id);

#define CFLOAT(NAME)          config_get_float(CN(NAME))
#define CCOL(NAME)            config_get_color(CN(NAME))
#define CCOLTX(VALUE,NAME)    config_color_transform(VALUE,CN(NAME))
#define CCOL32(NAME)          ImGui::GetColorU32(CCOL(NAME))
#define CKEY(NAME)            config_get_key(CN(NAME))


struct keyjazz_keymap {
	ImGuiKey keycode;
	int note;
};

#define KEYJAZZ1_KEYMAP_US "q2w3er5t6y7ui9o0p"
#define KEYJAZZ0_KEYMAP_US "zsxdcvgbhnjm"

#define KEYJAZZ1_KEYMAP_DE "q2w3er5t6z7ui9o0p"
#define KEYJAZZ0_KEYMAP_DE "ysxdcvgbhnjm"

struct keyjazz_keymap* get_keyjazz_keymap(int index);

void config_init(void);

// state defaults
#define C_DEFAULT_BEAT_WIDTH_PX (24)
#define C_DEFAULT_KEY_HEIGHT_PX (12)

int config_get_soundfont_count(void);
char* config_get_soundfont_path(int index);

#define CONFIG_H
#endif
