#ifndef MIID_H

#include "imgui.h"

void miid_init(int argc, char** argv, float sample_rate);
void miid_audio_callback(float* stream, int n_frames);
bool miid_frame(void* usr, bool request_close);

void miidhost_create_window(void* usr, ImFontAtlas* shared_font_atlas);

#define MIID_H
#endif
