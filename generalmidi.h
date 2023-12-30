#ifndef GENERALMIDI_H

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
	EFFECT3_DEPTH          = 93,
	RESET_ALL_CONTROLLERS  = 121,
};

#define EMIT_PROGRAMS                                          \
	PRG( 1    , "Acoustic Grand Piano"                   ) \
	PRG( 2    , "Bright Acoustic Piano"                  ) \
	PRG( 3    , "Electric Grand Piano"                   ) \
	PRG( 4    , "Honky-tonk Piano"                       ) \
	PRG( 5    , "Electric Piano 1 (Rhodes Piano)"        ) \
	PRG( 6    , "Electric Piano 2 (Chorused Piano)"      ) \
	PRG( 7    , "Harpsichord"                            ) \
	PRG( 8    , "Clavinet"                               ) \
	PRG( 9    , "Celesta"                                ) \
	PRG( 10   , "Glockenspiel"                           ) \
	PRG( 11   , "Music Box"                              ) \
	PRG( 12   , "Vibraphone"                             ) \
	PRG( 13   , "Marimba"                                ) \
	PRG( 14   , "Xylophone"                              ) \
	PRG( 15   , "Tubular Bells"                          ) \
	PRG( 16   , "Dulcimer (Santur)"                      ) \
	PRG( 17   , "Drawbar Organ (Hammond)"                ) \
	PRG( 18   , "Percussive Organ"                       ) \
	PRG( 19   , "Rock Organ"                             ) \
	PRG( 20   , "Church Organ"                           ) \
	PRG( 21   , "Reed Organ"                             ) \
	PRG( 22   , "Accordion (French)"                     ) \
	PRG( 23   , "Harmonica"                              ) \
	PRG( 24   , "Tango Accordion (Band neon)"            ) \
	PRG( 25   , "Acoustic Guitar (nylon)"                ) \
	PRG( 26   , "Acoustic Guitar (steel)"                ) \
	PRG( 27   , "Electric Guitar (jazz)"                 ) \
	PRG( 28   , "Electric Guitar (clean)"                ) \
	PRG( 29   , "Electric Guitar (muted)"                ) \
	PRG( 30   , "Overdriven Guitar"                      ) \
	PRG( 31   , "Distortion Guitar"                      ) \
	PRG( 32   , "Guitar harmonics"                       ) \
	PRG( 33   , "Acoustic Bass"                          ) \
	PRG( 34   , "Electric Bass (fingered)"               ) \
	PRG( 35   , "Electric Bass (picked)"                 ) \
	PRG( 36   , "Fretless Bass"                          ) \
	PRG( 37   , "Slap Bass 1"                            ) \
	PRG( 38   , "Slap Bass 2"                            ) \
	PRG( 39   , "Synth Bass 1"                           ) \
	PRG( 40   , "Synth Bass 2"                           ) \
	PRG( 41   , "Violin"                                 ) \
	PRG( 42   , "Viola"                                  ) \
	PRG( 43   , "Cello"                                  ) \
	PRG( 44   , "Contrabass"                             ) \
	PRG( 45   , "Tremolo Strings"                        ) \
	PRG( 46   , "Pizzicato Strings"                      ) \
	PRG( 47   , "Orchestral Harp"                        ) \
	PRG( 48   , "Timpani"                                ) \
	PRG( 49   , "String Ensemble 1 (strings)"            ) \
	PRG( 50   , "String Ensemble 2 (slow strings)"       ) \
	PRG( 51   , "SynthStrings 1"                         ) \
	PRG( 52   , "SynthStrings 2"                         ) \
	PRG( 53   , "Choir Aahs"                             ) \
	PRG( 54   , "Voice Oohs"                             ) \
	PRG( 55   , "Synth Voice"                            ) \
	PRG( 56   , "Orchestra Hit"                          ) \
	PRG( 57   , "Trumpet"                                ) \
	PRG( 58   , "Trombone"                               ) \
	PRG( 59   , "Tuba"                                   ) \
	PRG( 60   , "Muted Trumpet"                          ) \
	PRG( 61   , "French Horn"                            ) \
	PRG( 62   , "Brass Section"                          ) \
	PRG( 63   , "SynthBrass 1"                           ) \
	PRG( 64   , "SynthBrass 2"                           ) \
	PRG( 65   , "Soprano Sax"                            ) \
	PRG( 66   , "Alto Sax"                               ) \
	PRG( 67   , "Tenor Sax"                              ) \
	PRG( 68   , "Baritone Sax"                           ) \
	PRG( 69   , "Oboe"                                   ) \
	PRG( 70   , "English Horn"                           ) \
	PRG( 71   , "Bassoon"                                ) \
	PRG( 72   , "Clarinet"                               ) \
	PRG( 73   , "Piccolo"                                ) \
	PRG( 74   , "Flute"                                  ) \
	PRG( 75   , "Recorder"                               ) \
	PRG( 76   , "Pan Flute"                              ) \
	PRG( 77   , "Blown Bottle"                           ) \
	PRG( 78   , "Shakuhachi"                             ) \
	PRG( 79   , "Whistle"                                ) \
	PRG( 80   , "Ocarina"                                ) \
	PRG( 81   , "Lead 1 (square wave)"                   ) \
	PRG( 82   , "Lead 2 (sawtooth wave)"                 ) \
	PRG( 83   , "Lead 3 (calliope)"                      ) \
	PRG( 84   , "Lead 4 (chiffer)"                       ) \
	PRG( 85   , "Lead 5 (charang)"                       ) \
	PRG( 86   , "Lead 6 (voice solo)"                    ) \
	PRG( 87   , "Lead 7 (fifths)"                        ) \
	PRG( 88   , "Lead 8 (bass + lead)"                   ) \
	PRG( 89   , "Pad 1 (new age Fantasia)"               ) \
	PRG( 90   , "Pad 2 (warm)"                           ) \
	PRG( 91   , "Pad 3 (polysynth)"                      ) \
	PRG( 92   , "Pad 4 (choir space voice)"              ) \
	PRG( 93   , "Pad 5 (bowed glass)"                    ) \
	PRG( 94   , "Pad 6 (metallic pro)"                   ) \
	PRG( 95   , "Pad 7 (halo)"                           ) \
	PRG( 96   , "Pad 8 (sweep)"                          ) \
	PRG( 97   , "FX 1 (rain)"                            ) \
	PRG( 98   , "FX 2 (soundtrack)"                      ) \
	PRG( 99   , "FX 3 (crystal)"                         ) \
	PRG( 100  , "FX 4 (atmosphere)"                      ) \
	PRG( 101  , "FX 5 (brightness)"                      ) \
	PRG( 102  , "FX 6 (goblins)"                         ) \
	PRG( 103  , "FX 7 (echoes, drops)"                   ) \
	PRG( 104  , "FX 8 (sci-fi, star theme)"              ) \
	PRG( 105  , "Sitar"                                  ) \
	PRG( 106  , "Banjo"                                  ) \
	PRG( 107  , "Shamisen"                               ) \
	PRG( 108  , "Koto"                                   ) \
	PRG( 109  , "Kalimba"                                ) \
	PRG( 110  , "Bag pipe"                               ) \
	PRG( 111  , "Fiddle"                                 ) \
	PRG( 112  , "Shanai"                                 ) \
	PRG( 113  , "Tinkle Bell"                            ) \
	PRG( 114  , "Agogo"                                  ) \
	PRG( 115  , "Steel Drums"                            ) \
	PRG( 116  , "Woodblock"                              ) \
	PRG( 117  , "Taiko Drum"                             ) \
	PRG( 118  , "Melodic Tom"                            ) \
	PRG( 119  , "Synth Drum"                             ) \
	PRG( 120  , "Reverse Cymbal"                         ) \
	PRG( 121  , "Guitar Fret Noise"                      ) \
	PRG( 122  , "Breath Noise"                           ) \
	PRG( 123  , "Seashore"                               ) \
	PRG( 124  , "Bird Tweet"                             ) \
	PRG( 125  , "Telephone Ring"                         ) \
	PRG( 126  , "Helicopter"                             ) \
	PRG( 127  , "Applause"                               ) \
	PRG( 128  , "Gunshot"                                )

#define EMIT_KIT_PROGRAMS              \
	PRG( 1    , "Standard Kit"   ) \
	PRG( 9    , "Room Kit"       ) \
	PRG( 17   , "Power Kit"      ) \
	PRG( 25   , "Electronic Kit" ) \
	PRG( 26   , "TR-808 Kit"     ) \
	PRG( 33   , "Jazz Kit"       ) \
	PRG( 41   , "Brush Kit"      ) \
	PRG( 49   , "Orchestra Kit"  ) \
	PRG( 57   , "Sound FX Kit"   )

#define EMIT_DRUM_KEYS                   \
	KEY( 35,  "Acoustic Bass Drum" ) \
	KEY( 36,  "Bass Drum 1"        ) \
	KEY( 37,  "Side Stick"         ) \
	KEY( 38,  "Acoustic Snare"     ) \
	KEY( 39,  "Hand Clap"          ) \
	KEY( 40,  "Electric Snare"     ) \
	KEY( 41,  "Low Floor Tom"      ) \
	KEY( 42,  "Closed Hi Hat"      ) \
	KEY( 43,  "High Floor Tom"     ) \
	KEY( 44,  "Pedal Hi-Hat"       ) \
	KEY( 45,  "Low Tom"            ) \
	KEY( 46,  "Open Hi-Hat"        ) \
	KEY( 47,  "Low-Mid Tom"        ) \
	KEY( 48,  "Hi Mid Tom"         ) \
	KEY( 49,  "Crash Cymbal 1"     ) \
	KEY( 50,  "High Tom"           ) \
	KEY( 51,  "Ride Cymbal 1"      ) \
	KEY( 52,  "Chinese Cymbal"     ) \
	KEY( 53,  "Ride Bell"          ) \
	KEY( 54,  "Tambourine"         ) \
	KEY( 55,  "Splash Cymbal"      ) \
	KEY( 56,  "Cowbell"            ) \
	KEY( 57,  "Crash Cymbal 2"     ) \
	KEY( 58,  "Vibraslap"          ) \
	KEY( 59,  "Ride Cymbal 2"      ) \
	KEY( 60,  "Hi Bongo"           ) \
	KEY( 61,  "Low Bongo"          ) \
	KEY( 62,  "Mute Hi Conga"      ) \
	KEY( 63,  "Open Hi Conga"      ) \
	KEY( 64,  "Low Conga"          ) \
	KEY( 65,  "High Timbale"       ) \
	KEY( 66,  "Low Timbale"        ) \
	KEY( 67,  "High Agogo"         ) \
	KEY( 68,  "Low Agogo"          ) \
	KEY( 69,  "Cabasa"             ) \
	KEY( 70,  "Maracas"            ) \
	KEY( 71,  "Short Whistle"      ) \
	KEY( 72,  "Long Whistle"       ) \
	KEY( 73,  "Short Guiro"        ) \
	KEY( 74,  "Long Guiro"         ) \
	KEY( 75,  "Claves"             ) \
	KEY( 76,  "Hi Wood Block"      ) \
	KEY( 77,  "Low Wood Block"     ) \
	KEY( 78,  "Mute Cuica"         ) \
	KEY( 79,  "Open Cuica"         ) \
	KEY( 80,  "Mute Triangle"      ) \
	KEY( 81,  "Open Triangle"      )

#define GENERALMIDI_H
#endif
