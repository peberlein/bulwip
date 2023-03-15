/*
 *  sdl.c - VDP 9918A functions using SDL
 *
 * Copyright (c) 2023 Pete Eberlein
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "cpu.h"

// audio player
#include "player.h"


// enabled in Makefile
#ifdef ENABLE_CRT
//#include "NTSC-CRT-v2/crt_core.h"
#include "NTSC-CRT/crt.h"
#endif


void snd_w(unsigned char byte)
{
	// FIXME sound bytes should be written to a buffer with cpu clock counts,
	// so that the register changes can simulated as the audio is generated,
	// TODO isn't there also a CRU bit that controls audio (cassette tape?)
	//fprintf(stderr, "sound %02x\n", byte);
	snd(byte);
}
static Uint64 next_time = 0;

static void my_audio_callback(void *userdata, Uint8 *stream, int len)
{
	Uint32 now = SDL_GetTicks();
	int time_left = (next_time >> 32) - now;
	if (time_left <= -50) {
		// render loop is paused or window being moved/resized
		memset(stream, 128, len); // silence
	} else {
		// For AUDIO_U8, len is number of samples to generate
		update(stream, 0, len);
	}
}


// This is the screen texture
// Normally render 320x240, 80-col text mode 640x240, CRT mode 640x480
static const int texture_width = 640, texture_height = 480;
static SDL_Texture *texture = NULL;
static int texture_len = 0; // will be 320 for normal, 640 for 80-col text mode
#ifdef ENABLE_CRT
static void *crt_src = NULL;
static void *crt_dest = NULL;
#endif

// NOTE: pixels is write-only
void vdp_lock_texture(int line, int len, void**pixels, int *pitch)
{
	texture_len = len; // determines the display width of the texture 
#ifdef ENABLE_CRT
	if (config_crt_filter == 2 && crt_src != NULL) {
		*pixels = crt_src + texture_width * 4 * line;
		*pitch = texture_width * 4;
	} else
#endif
	if (texture) {
		SDL_Rect rect = {0, line, len, 1};
		SDL_LockTexture(texture, &rect, pixels, pitch);
	}
}

void vdp_unlock_texture(void)
{
#ifdef ENABLE_CRT
	if (config_crt_filter == 2 && crt_src != NULL) {
		// nothing to do, but don't unlock texture we haven't locked
	} else
#endif
	if (texture) {
		SDL_UnlockTexture(texture);
	}
}


static unsigned char *text_pat;

void vdp_text_pat(unsigned char *pat)
{
	text_pat = pat;
}

static SDL_Texture *debug_texture = NULL;

extern unsigned int palette[16]; // bulwip.c

#define AMSK 0xff000000
#define RMSK 0x00ff0000
#define GMSK 0x0000ff00
#define BMSK 0x000000ff


void vdp_text_window(const char *line, int w, int h, int x, int y, int highlight_line)
{
	char fg_color = 1; //15;
	char bg_color = 14; //4;
	uint32_t bg = palette[highlight_line == 0 ? fg_color : bg_color] | AMSK;
	uint32_t fg = palette[highlight_line == 0 ? bg_color : fg_color] | AMSK;
	uint32_t *pixels;
	int pitch = 0;
	const char *start = line;
	struct SDL_Rect rect = {
		.x = x,
		.y = y,
		.w = w * 6,
		.h = h * 8,
	};

	if (!debug_texture) return;
	SDL_LockTexture(debug_texture, &rect, (void**)&pixels, &pitch);

	//pixels += x + y * (pitch/4);
	for (unsigned int j = 0; j < h*8; j++) {
		if ((j&7) == 7) {
			if (j/8 == highlight_line-1) {
				bg = palette[fg_color] | AMSK;  // inverted
				fg = palette[bg_color] | AMSK;
			}
#if 0
			for (unsigned int i = 0; i < w*6; i++)
				*pixels++ = bg;
			//line += w;
			start = line;
			text_pat -= 7;
			pixels += (pitch/4) - (w*6);

			if (j/8 == highlight_line) {
				bg = palette[bg_color] | AMSK;  // normal
				fg = palette[fg_color] | AMSK;
			}
			continue;
#endif
		}
		line = start;
		for (unsigned char i = 0; i < w; i++) {
			unsigned char ch = *line++;
			if (ch == '\n' || ch == '\r' || ch == 0) {
				for (; i < w; i++) {
					for (unsigned char m = 0x80; m != 2; m >>= 1)
						*pixels++ = bg;
				}
				break;
			}
			if (ch >= 'a' && ch <= 'z') {
				static const u8 lowercase[] = {
					0x00,0x38,0x04,0x3C,0x44,0x4C,0x34,0x00, // a
					0x40,0x58,0x64,0x44,0x44,0x44,0x78,0x00, // b
					0x00,0x38,0x44,0x40,0x40,0x44,0x38,0x00, // c
					0x04,0x34,0x4c,0x44,0x44,0x44,0x3C,0x00, // d
					0x00,0x38,0x44,0x7C,0x40,0x44,0x38,0x00, // e
					0x18,0x24,0x20,0x78,0x20,0x20,0x20,0x00, // f
					0x00,0x38,0x44,0x44,0x44,0x3C,0x44,0x38, // g
					0x40,0x58,0x64,0x44,0x44,0x44,0x44,0x00, // h
					0x10,0x00,0x30,0x10,0x10,0x10,0x38,0x00, // i
					0x08,0x00,0x18,0x08,0x08,0x08,0x48,0x30, // j
					0x40,0x48,0x50,0x60,0x50,0x48,0x44,0x00, // k
					0x30,0x10,0x10,0x10,0x10,0x10,0x38,0x00, // l
					0x00,0x68,0x54,0x54,0x54,0x44,0x44,0x00, // m
					0x00,0x58,0x64,0x44,0x44,0x44,0x44,0x00, // n
					0x00,0x38,0x44,0x44,0x44,0x44,0x38,0x00, // o
					0x00,0x78,0x44,0x44,0x44,0x78,0x40,0x40, // p
					0x00,0x38,0x44,0x44,0x44,0x3C,0x04,0x04, // q
					0x00,0x58,0x64,0x40,0x40,0x40,0x40,0x00, // r
					0x00,0x38,0x44,0x30,0x08,0x44,0x38,0x00, // s
					0x20,0x78,0x20,0x20,0x20,0x24,0x18,0x00, // t
					0x00,0x44,0x44,0x44,0x44,0x4C,0x34,0x00, // u
					0x00,0x44,0x44,0x44,0x28,0x28,0x10,0x00, // v
					0x00,0x44,0x44,0x54,0x54,0x54,0x28,0x00, // w
					0x00,0x44,0x28,0x10,0x10,0x28,0x44,0x00, // x
					0x00,0x44,0x44,0x44,0x4C,0x34,0x44,0x38, // y
					0x00,0x7C,0x08,0x10,0x20,0x40,0x7C,0x00, // z
				};
				ch = lowercase[(ch-'a') * 8 + (j&7)];
			} else if (ch <= ' ' || (j&7) == 7) {
				ch = 0;
			} else {
				ch = text_pat[ch * 7];
			}
			for (unsigned char m = 0x80; m != 2; m >>= 1)
				*pixels++ = ch & m ? fg : bg;
		}
		while (line[-1] != 0 && line[-1] != '\n' && line[0] != '\r') {
			line++;
		}
		if (line[-1] == 0) {
			line--; // stay on NUL
		} else if ((line[-1] == '\n' && line[0] == '\r') ||
			(line[-1] == '\r' && line[0] == '\n')) {
			line++; // skip pair
		}

		pixels += (pitch/4) - (w*6);
		text_pat++;

		if ((j&7) == 7) {
			//line += w;
			start = line;
			text_pat -= 8;
			if (j/8 == highlight_line) {
				bg = palette[bg_color] | AMSK;  // normal
				fg = palette[fg_color] | AMSK;
			}
		}
	}
	SDL_UnlockTexture(debug_texture);
}

void vdp_text_clear(int x, int y, int w, int h, unsigned int color)
{
	SDL_Surface *surface;
	struct SDL_Rect rect = {
		.x = x,
		.y = y,
		.w = w * 6,
		.h = h * 8,
	};
	void *pixels;
	int pitch;

	if (!debug_texture) return;
	//SDL_LockTextureToSurface(debug_texture, &rect, &surface); // requires SDL 2.0.12
	SDL_LockTexture(debug_texture, &rect, &pixels, &pitch);
	surface = SDL_CreateRGBSurfaceFrom(pixels, rect.w, rect.h, 32, pitch, RMSK, GMSK, BMSK, AMSK);
	if (surface) {
		SDL_FillRect(surface, NULL, color);
		SDL_FreeSurface(surface);
	}
	SDL_UnlockTexture(debug_texture);
}

void vdp_draw_graph(double *array)
{
	SDL_Surface *surface;
	int i, pk = -1;
	unsigned int color;
	void *pixels;
	int pitch;

	if (!debug_texture) return;
	//SDL_LockTextureToSurface(debug_texture, NULL, &surface); // requires SDL 2.0.12
	SDL_LockTexture(debug_texture, NULL, &pixels, &pitch);
	surface = SDL_CreateRGBSurfaceFrom(pixels, 640, 480, 32, pitch, RMSK, GMSK, BMSK, AMSK);
	if (surface) {
		SDL_FillRect(surface, NULL, AMSK);
		for (i = 0; i < 640; i++) {
			int n = (int)(array[i]*20);
			struct SDL_Rect rect = {
				.x = i,
				.y = 480-n,
				.w = 1,
				.h = n,
			};
			unsigned int color = 0xffffffff;
			if (pk == -1 && array[i] > array[i+1]) {
				color = AMSK | RMSK;
				pk = i;
			} else if (pk != -1 && array[i] < array[i+1]) {
				color = AMSK | GMSK;
				pk = -1;
			}
			SDL_FillRect(surface, &rect, color);
		}
		SDL_FreeSurface(surface);
	}
	SDL_UnlockTexture(debug_texture);
}


static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
#ifdef ENABLE_CRT
static struct CRT crt;
#define CRT_W ((640)*1)
#define CRT_H ((480)*1)
#endif

static Uint64 ticks_per_frame = 0; // actually ticks per frame << 32
static int first_tick = 0;
static unsigned int frames = 0;
static unsigned int scale_w = 640, scale_h = 480; // window size
static unsigned int config_fullscreen = 0;
int menu_active = 0;
int current_mfps = 0;

void vdp_set_fps(int mfps /* fps*1000 */)
{
	current_mfps = mfps;
	if (mfps == 0) {
		ticks_per_frame = 0; // uncapped framerate
	} else {
		// This can be used to set frame rate to a more exact fraction
		// Or with another divider to speed up/slow down emulation
		ticks_per_frame = ((Uint64)1000000 << 32) / mfps;
	}
}

void vdp_window_scale(int scale)
{
	scale_w = 320 * scale;
	scale_h = 240 * scale;
	if (window)
		SDL_SetWindowSize(window, scale_w, scale_h);
}

void vdp_set_filter(void)
{
	if (texture) SDL_DestroyTexture(texture);
	if (config_crt_filter == 0) {
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
	} else {
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
	}
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, texture_width, texture_height);

	redraw_vdp(); // redraw the screen texture
}

void vdp_init(void)
{
	SDL_AudioSpec audio;
	int video = 1;

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL video: %s", SDL_GetError());
		video = 0;
	}

	if (SDL_Init(SDL_INIT_AUDIO) < 0) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL audio: %s", SDL_GetError());
	} else {

		audio.callback = my_audio_callback;
		audio.userdata = NULL;
		audio.freq = SAMPLE_FREQUENCY;
		audio.format = AUDIO_U8;
		audio.channels = 1;
		audio.samples = 256; // low-latency

		if (SDL_OpenAudio(&audio, NULL) < 0) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't open SDL audio: %s", SDL_GetError());
		}
	}

	if (video) {
		window = SDL_CreateWindow("BuLWiP TI-99/4A - Esc for menu",
				SDL_WINDOWPOS_UNDEFINED,
				SDL_WINDOWPOS_UNDEFINED,
				scale_w,
				scale_h,
				SDL_WINDOW_RESIZABLE);

		renderer = SDL_CreateRenderer(window, -1, 0);

		config_crt_filter = 0;
		vdp_set_filter(); // this creates the "texture" texture

		debug_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING, 640, 480);
		SDL_SetTextureBlendMode(debug_texture, SDL_BLENDMODE_BLEND);
#ifdef ENABLE_CRT
		crt_src = calloc(texture_width * 240, 4);
		crt_dest = calloc(CRT_W * CRT_H, 4);
		crt_init(&crt, CRT_W, CRT_H, crt_dest);
#endif
	}


	if (0) { // print the palette
		int i;
		for (i = 0; i < 16; i++) {
			fprintf(stderr, "%06x\n", palette[i]);
		}
	}
	first_tick = SDL_GetTicks();

	vdp_set_fps(NTSC_FPS);
	vdp_text_clear(0, 0, 640/6+1, 480/8, AMSK); // clear debug window

	if (window) {
		SDL_PauseAudio(0); // start playing
	}

}

void vdp_done(void)
{
	fprintf(stderr, "SDL_QUIT %f fps\n", frames*1000.0/(SDL_GetTicks()-first_tick));
	if (texture) SDL_DestroyTexture(texture);
#ifdef ENABLE_CRT
	free(crt_src);
	free(crt_dest);
#endif
	if (renderer) SDL_DestroyRenderer(renderer);
	if (window) SDL_CloseAudio();
	SDL_Quit();
}

// returns -1 if the SDL window is closed, otherwise 0
int vdp_update(void)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT) {
			return -1;
		} else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
			SDL_KeyboardEvent *key = &event.key;
			Uint16 mod = key->keysym.mod;
			int kdn = event.type == SDL_KEYDOWN;
			int k = -1;
			int alphalock = 0; //kdn && (mod & KMOD_SHIFT) ? TI_ALPHALOCK : 0;


			// use sym for virtual key representation (mapped keyboard layout)
			switch (key->keysym.sym) {
			case SDLK_ESCAPE: k = TI_MENU; break;
			case SDLK_EQUALS: k = TI_EQUALS; break;
			case SDLK_SPACE:  k = TI_SPACE; break;
			case SDLK_RETURN: k = TI_ENTER; break;
			case SDLK_LALT:   k = TI_FCTN; break;
			case SDLK_RALT:   k = TI_FCTN; break;
			case SDLK_LSHIFT: k = TI_SHIFT; break;
			case SDLK_RSHIFT: k = TI_SHIFT; break;
			case SDLK_LCTRL:  k = TI_CTRL; break;
			case SDLK_RCTRL:  k = TI_CTRL; break;

			case SDLK_PERIOD: k = TI_PERIOD; break;
			case SDLK_l:      k = TI_L+alphalock; break;
			case SDLK_o:      k = TI_O+alphalock; break;
			case SDLK_9:      k = TI_9; break;
			case SDLK_2:      k = TI_2; break;
			case SDLK_s:      k = TI_S+alphalock; break;
			case SDLK_w:      k = TI_W+alphalock; break;
			case SDLK_x:      k = TI_X+alphalock; break;

			case SDLK_COMMA:  k = TI_COMMA; break;
			case SDLK_k:      k = TI_K+alphalock; break;
			case SDLK_i:      k = TI_I+alphalock; break;
			case SDLK_8:      k = TI_8; break;
			case SDLK_3:      k = TI_3; break;
			case SDLK_d:      k = TI_D+alphalock; break;
			case SDLK_e:      k = TI_E+alphalock; break;
			case SDLK_c:      k = TI_C+alphalock; break;

			case SDLK_m:      k = TI_M+alphalock; break;
			case SDLK_j:      k = TI_J+alphalock; break;
			case SDLK_u:      k = TI_U+alphalock; break;
			case SDLK_7:      k = TI_7; break;
			case SDLK_4:      k = TI_4; break;
			case SDLK_f:      k = TI_F+alphalock; break;
			case SDLK_r:      k = TI_R+alphalock; break;
			case SDLK_v:      k = TI_V+alphalock; break;

			case SDLK_n:      k = TI_N+alphalock; break;
			case SDLK_h:      k = TI_H+alphalock; break;
			case SDLK_y:      k = TI_Y+alphalock; break;
			case SDLK_6:      k = TI_6; break;
			case SDLK_5:      k = TI_5; break;
			case SDLK_g:      k = TI_G+alphalock; break;
			case SDLK_t:      k = TI_T+alphalock; break;
			case SDLK_b:      k = TI_B+alphalock; break;

			case SDLK_SLASH:  k = TI_SLASH; break;
			case SDLK_SEMICOLON: k = TI_SEMICOLON; break;
			case SDLK_p:      k = TI_P+alphalock; break;
			case SDLK_0:      k = TI_0; break;
			case SDLK_1:      k = TI_1; break;
			case SDLK_a:      k = TI_A+alphalock; break;
			case SDLK_q:      k = TI_Q+alphalock; break;
			case SDLK_z:      k = TI_Z+alphalock; break;

			case SDLK_TAB:    k = TI_FIRE1; break;
			case SDLK_LEFT:   k = TI_LEFT1; break;
			case SDLK_RIGHT:  k = TI_RIGHT1; break;
			case SDLK_DOWN:   k = TI_DOWN1; break;
			case SDLK_UP:     k = TI_UP1; break;

			case SDLK_BACKSPACE: k = TI_S | TI_ADDFCTN; break;
			case SDLK_DELETE: k = TI_1 | TI_ADDFCTN; break;
			case SDLK_INSERT:
				if ((mod & KMOD_SHIFT) && kdn) {
					char *text = SDL_GetClipboardText();
					if (text && text[0]) paste_text(text, current_mfps);
					SDL_free(text);
				} else {
					k = TI_2 | TI_ADDFCTN;
				}
				break;
			case SDLK_BACKQUOTE: k = TI_C | TI_ADDFCTN; break;
			case SDLK_LEFTBRACKET: k = (mod & KMOD_SHIFT ? TI_G : TI_R) | TI_ADDFCTN; break;
			case SDLK_RIGHTBRACKET: k = (mod & KMOD_SHIFT ? TI_F : TI_T) | TI_ADDFCTN; break;
			case SDLK_BACKSLASH: k = (mod & KMOD_SHIFT ? TI_A : TI_Z) | TI_ADDFCTN; break;
			case SDLK_UNDERSCORE: k = TI_U | TI_ADDFCTN; break;
			case SDLK_QUESTION: k = TI_I | TI_ADDFCTN; break;
			case SDLK_QUOTE: k = (mod & KMOD_SHIFT ? TI_P : TI_O) | TI_ADDFCTN; break;
			case SDLK_QUOTEDBL: k = TI_P | TI_ADDFCTN; break;
			case SDLK_MINUS: k = TI_SLASH | TI_ADDSHIFT; break;
			case SDLK_PLUS: k = TI_EQUALS | TI_ADDSHIFT; break;

			case SDLK_KP_0: k = (mod & KMOD_NUM) ? TI_0 : -1; break;
			case SDLK_KP_1: k = (mod & KMOD_NUM) ? TI_1 : -1; break;
			case SDLK_KP_2: k = (mod & KMOD_NUM) ? TI_2 : -1; break;
			case SDLK_KP_3: k = (mod & KMOD_NUM) ? TI_3 : -1; break;
			case SDLK_KP_4: k = (mod & KMOD_NUM) ? TI_4 : -1; break;
			case SDLK_KP_5: k = (mod & KMOD_NUM) ? TI_5 : -1; break;
			case SDLK_KP_6: k = (mod & KMOD_NUM) ? TI_6 : -1; break;
			case SDLK_KP_7: k = (mod & KMOD_NUM) ? TI_7 : -1; break;
			case SDLK_KP_8: k = (mod & KMOD_NUM) ? TI_8 : -1; break;
			case SDLK_KP_9: k = (mod & KMOD_NUM) ? TI_9 : -1; break;
			case SDLK_KP_PERIOD: k = (mod & KMOD_NUM) ? TI_PERIOD : -1; break;
			case SDLK_KP_MULTIPLY: k = TI_8 | TI_ADDSHIFT; break;
			case SDLK_KP_DIVIDE: k = TI_SLASH; break;
			case SDLK_KP_MINUS: k = TI_SLASH | TI_ADDSHIFT; break;
			case SDLK_KP_PLUS: k = TI_EQUALS | TI_ADDSHIFT; break;
			case SDLK_KP_ENTER: k = TI_ENTER; break;

			case SDLK_PAGEUP:   k = TI_PAGEUP; break;
			case SDLK_PAGEDOWN: k = TI_PAGEDN; break;
			case SDLK_HOME:
				if (mod == 0) {
					k = TI_HOME;
				} else if (kdn && (mod & KMOD_CTRL)) {
					debug_en = !debug_en;
					if (!debug_en)
						debug_break = DEBUG_RUN;
				}
				break;
			case SDLK_END:
				if (kdn && mod == 0) k = TI_END;
				break;

			case SDLK_F1:
				if (debug_en) {
					if (kdn) debug_break = !debug_break;
				} else {
					k = TI_1+TI_ADDFCTN;
				}
				break;
			case SDLK_F2:
				if (debug_en) {
					if (kdn) debug_break = (mod & KMOD_SHIFT) ? DEBUG_FRAME_STEP : DEBUG_SINGLE_STEP;
				} else {
					k = TI_2+TI_ADDFCTN;
				}
				break;
			case SDLK_F3: k = TI_3+TI_ADDFCTN; break;
			case SDLK_F4: k = TI_4+TI_ADDFCTN; break;
			case SDLK_F5: k = TI_5+TI_ADDFCTN; break;
			case SDLK_F6: k = TI_6+TI_ADDFCTN; break;
			case SDLK_F7: k = TI_7+TI_ADDFCTN; break;
			case SDLK_F8: k = TI_8+TI_ADDFCTN; break;
			case SDLK_F9: k = TI_9+TI_ADDFCTN; break;
			case SDLK_F10: k = TI_0+TI_ADDFCTN; break;


			case SDLK_F11: if (kdn) SDL_SetWindowFullscreen(window, (config_fullscreen ^= SDL_WINDOW_FULLSCREEN)); break;
			case SDLK_F12: if (!kdn) break; if (mod & KMOD_CTRL) reset(); else debug_en =! debug_en; break;
			//case SDLK_F5: if (kdn) config_crt_filter = (config_crt_filter + 1) % 3; vdp_set_filter(); break;
			default: break;
			}

			//fprintf(stderr, "key %d mod %x val %d row %d col=%d\n", key->keysym.scancode, mod, val, row, col);
			if (k != -1) {
				if (k != TI_SHIFT && k != TI_CTRL && k != TI_FCTN && kdn) {
					// a non-modifier key pressed for ui
					set_ui_key(k & (TI_ADDCTRL | TI_ADDFCTN | TI_ADDSHIFT) ?
						k : // already has modifiers
						k | (mod & KMOD_CTRL ? TI_ADDCTRL : 0)
						  | (mod & KMOD_ALT ? TI_ADDFCTN : 0)
						  | (mod & KMOD_SHIFT ? TI_ADDSHIFT : 0));
				}
				if (k == TI_SHIFT) {
					// modifier key changed, clear any modifiable keys
					set_key(kdn ? TI_R : TI_G, 0);
					set_key(kdn ? TI_T : TI_F, 0);
					set_key(kdn ? TI_Z : TI_A, 0);
					set_key(kdn ? TI_O : TI_P, 0);
				}
				if (!kdn) set_key(k & 0x3f, 0); // on keyup, set meta after
				if (k & TI_ADDSHIFT) {
					set_key(TI_SHIFT, kdn || (mod & KMOD_SHIFT));
				}
				if (k & TI_ADDFCTN) {
					set_key(TI_FCTN, kdn || (mod & KMOD_ALT));
					set_key(TI_SHIFT, !kdn && (mod & KMOD_SHIFT));
				}
				if (kdn) {
					if ((k & (TI_ADDCTRL|TI_ADDSHIFT|TI_ADDFCTN)) == 0) {
						// clear meta keys that are not actually held
						set_key(TI_CTRL, !!(mod & KMOD_CTRL));
						set_key(TI_FCTN, !!(mod & KMOD_ALT));
						set_key(TI_SHIFT, !!(mod & KMOD_SHIFT));
					}
					set_key(k & (0x3f|TI_ALPHALOCK), 1); // on keydown, set meta first
				}

			}

		} else {
			//printf("event.type=%x\n", event.type);
		}
	}

	if (renderer) {
		SDL_Rect src = {.x = 0, .y = 0, .w = texture_len, .h = 240};
		SDL_Rect dst = {.x = 0, .y = 0, .w = scale_w, .h = scale_h};

#ifdef ENABLE_CRT
		if (config_crt_filter == 2) {
			struct NTSC_SETTINGS ntsc = {
				.w = src.w,
				.h = src.h - 4,  // FIXME: why does this fix blurry lines?
				.raw = 0, // scale image to fit monitor
				.as_color = 1, // full color
				.field = 0, //(frames & 1), // 0 = even, 1 = odd
#if !defined(CRT_MAJOR) || CRT_MAJOR < 2
				.cc = { 0, 1, 0, -1 },
				.ccs = 1,
#endif
			};
			int noise = 4;
			int pitch = 640*4;
			void *pixels;

			ntsc.rgb = crt_src;
#if defined(CRT_MAJOR) && CRT_MAJOR == 2
			crt_modulate(&crt, &ntsc);
#else
			ntsc.pitch = pitch/4;
			crt_2ntsc(&crt, &ntsc);
#endif

			src.w = CRT_W;
			src.h = CRT_H;
			crt.outh = CRT_H;

			SDL_LockTexture(texture, &src, (void**)&pixels, &pitch);

			crt.out = crt_dest;
#if defined(CRT_MAJOR) && CRT_MAJOR == 2
			crt_demodulate(&crt, noise);
#else
			crt.outpitch = pitch/4;
			crt_draw(&crt, noise);
#endif
			memcpy(pixels, crt.out, src.h * pitch);
			SDL_UnlockTexture(texture);
		}
#endif
		if (debug_en) {
			SDL_Rect rect = {.x = 0, .y = 0, .w = scale_w/2, .h = scale_h/2};
			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, debug_texture, NULL, NULL);
			SDL_RenderCopy(renderer, texture, &src, &rect);
		} else {
			SDL_RenderCopy(renderer, texture, &src, NULL);
		}
		if (menu_active) {
			// menu overlay
			SDL_Rect src = {.x = 0, .y = 0, .w = 320, .h = 240};
			SDL_RenderCopy(renderer, debug_texture, &src, NULL);
		}

		// TODO use SDL_GetTicks() to determine actual framerate and 
		// dupe/drop frames to achieve desired VDP freq
		SDL_RenderPresent(renderer);
		if (ticks_per_frame != 0) { // cap frame rate, approximately
			Uint32 now = SDL_GetTicks();
			int time_left = (next_time >> 32) - now;

			if (next_time == 0 || time_left <= 0) {
				next_time = ((Uint64)now << 32);
			} else {
				SDL_Delay(time_left);
			}
			next_time += ticks_per_frame;
		} else {
			// still needed for my_audio_callback()
			next_time = ((Uint64)SDL_GetTicks() << 32);
		}
		frames++;
		//if (frames == 600) {
		//	fprintf(stderr, "600 frames %f fps\n", frames*1000.0/(SDL_GetTicks()-first_tick));
		//	return -1;
		//}
	}
	return 0;
}


