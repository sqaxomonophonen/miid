#ifndef CONFIG_H

// TODO keybinding
// TODO mouse wheel sensitivity?

// default GUI size; overridable with MIID_SZ environment variable
#define C_DEFAULT_SIZE (20)

// bake path to default font, instead of one built into binary. the MIID_TTF
// environment variable still overrides both.
//   #define C_TTF "/usr/share/fonts/truetype/..."

#define C_DEFAULT_BEAT_WIDTH_PX (24)

#define C_TIMELINE_ZOOM_SENSITIVITY (1.05)


///////////////////////////////////////////////////////////////////////////////
// colors
#define RGB(v)  ImColor(                 ((v)>>16)&0xff, ((v)>>8)&0xff, (v)&0xff ).Value
#define RGBA(v) ImColor( ((v)>>24)&0xff, ((v)>>16)&0xff, ((v)>>8)&0xff, (v)&0xff ).Value
#define C_TOGGLE_BUTTON_OFF_SCALAR        (0.3)
#define C_TOGGLE_BUTTON_HOVER_BRIGHTEN    (0.2)
#define C_TOGGLE_BUTTON_ACTIVE_BRIGHTEN   (0.5)

#define C_TRACKS_ROW_TOGGLE_COLOR     RGB(0x008020)

#define C_TRACK_ROW_EVEN_COLOR                        RGB(0x111111)
#define C_TRACK_ROW_ODD_COLOR                         RGB(0x000011)
#define C_TRACK_ROW_HOVER_ADD_COLOR                   RGB(0x111111)
#define C_TRACK_ROW_PRIMARY_SELECTION_ADD_COLOR       RGB(0x443355)
#define C_TRACK_ROW_SECONDARY_SELECTION_ADD_COLOR     RGB(0x001166)

#define C_TICK0_WIDTH (2.0)
#define C_TICK0_COLOR RGB(0xffffff)

#define C_TICKN_WIDTH (1.0)
#define C_TICKN_COLOR RGB(0x222222)

#define C_BAR_LABEL_COLOR    RGB(0xffff00)
#define C_TICK_LABEL_COLOR   RGB(0x555500)
#define C_TEMPO_LABEL_COLOR  RGB(0xcc77cc)

#define C_TIMESPAN_TOP_COLOR  RGB(0x554400)
#define C_TIMESPAN_DIM_COLOR  RGBA(0x55440040)

#define CONFIG_H
#endif
