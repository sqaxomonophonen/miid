#include <SDL.h>
#include "gl.h"

#include "stb_ds.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

#include "config.h"
#include "miid.h"

static SDL_AudioDeviceID audio_device;

static void audio_callback(void* usr, Uint8* stream, int len)
{
	miid_audio_callback((float*)stream, len / (2*sizeof(float)));
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

struct window {
	SDL_Window*   sdlwindow;
	SDL_GLContext glctx;
	ImGuiContext* imctx;
	void*         usr;
	bool          request_close;
};

struct window* window_arr;

void miidhost_create_window(void* usr, ImFontAtlas* shared_font_atlas)
{
	struct window w = {0};
	w.usr = usr;
	w.sdlwindow = SDL_CreateWindow(
		"MiiD",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		1920, 1080,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
	assert(w.sdlwindow != NULL);
	w.glctx = SDL_GL_CreateContext(w.sdlwindow);
	assert(w.glctx);
	w.imctx = ImGui::CreateContext(shared_font_atlas);
	ImGui::SetCurrentContext(w.imctx);
	ImGui_ImplSDL2_InitForOpenGL(w.sdlwindow, w.glctx);
	ImGui_ImplOpenGL2_Init();
	arrput(window_arr, w);
}

int main(int argc, char** argv)
{
	config_init();

	assert(SDL_Init(SDL_INIT_TIMER | SDL_INIT_AUDIO | SDL_INIT_VIDEO) == 0);
	atexit(SDL_Quit);

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 1);

	IMGUI_CHECKVERSION();

	SDL_AudioSpec have = {0}, want = {0};
	want.freq = 48000;
	want.format = AUDIO_F32;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio_callback;
	audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	bool have_audio = false;
	if (audio_device > 0) {
		have_audio = true;
		assert(have.channels == want.channels);
		assert(have.format == want.format);
		fprintf(stderr, "INFO: audio %dhz n=%d\n", have.freq, have.samples);
	} else {
		fprintf(stderr, "INFO: no audio\n");
	}

	miid_init(argc, argv, have_audio ? have.freq : 0);

	if (have_audio) {
		SDL_PauseAudioDevice(audio_device, 0);
	}

	bool exiting = false;
	while (!exiting) {
		const int n_windows = arrlen(window_arr);
		if (n_windows == 0) break;
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT) exiting = true;
			Uint32 window_id = get_event_window_id(&e);
			for (int i = 0; i < n_windows; i++) {
				struct window* w = &window_arr[i];
				if (SDL_GetWindowID(w->sdlwindow) != window_id) continue;
				if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE) {
					w->request_close = true;
				} else {
					ImGui::SetCurrentContext(w->imctx);
					ImGui_ImplSDL2_ProcessEvent(&e);
				}
				break;
			}
		}

		int n_delete = 0;
		for (int i = 0; i < n_windows; i++) {
			struct window* w = &window_arr[i];

			SDL_GL_MakeCurrent(w->sdlwindow, w->glctx);
			ImGui::SetCurrentContext(w->imctx);

			ImGuiIO& io = ImGui::GetIO();

			ImGui_ImplOpenGL2_NewFrame();
			ImGui_ImplSDL2_NewFrame();
			ImGui::NewFrame();

			const bool do_close = miid_frame(w->usr, w->request_close);

			ImGui::Render();
			glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
			SDL_GL_SwapWindow(w->sdlwindow);

			if (do_close) {
				ImGui::DestroyContext(w->imctx);
				SDL_GL_DeleteContext(w->glctx);
				SDL_DestroyWindow(w->sdlwindow);
				w->sdlwindow = NULL;
				n_delete++;
			} else {
				w->request_close = false;
			}
		}

		for (int i0 = 0; i0 < n_delete; i0++) {
			const int n = arrlen(window_arr);
			bool did_delete = false;
			for (int i1 = 0; i1 < n; i1++) {
				struct window* w = &window_arr[i1];
				if (w->sdlwindow != NULL) continue;
				arrdel(window_arr, i1);
				did_delete = true;
				break;
			}
			assert(did_delete);
		}
	}

	return EXIT_SUCCESS;
}
