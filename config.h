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

#define CONFIG_H
#endif
