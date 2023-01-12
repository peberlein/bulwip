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
#include <SDL.h>

// audio player
#include "player.h"


// clamp number between 0.0 and 1.0
#define CLAMP(x) ((x) < 0 ? 0 : (x) > 1 ? 1 : (x))
// take values from 0.0 to 1.0
#if 1
// YUV to RGB for analog TV
#define YUV2RGB(Y,Cb,Cr) (((int)(CLAMP(1.000*Y               +1.140*(Cb-0.5))*255)<<16) | \
                          ((int)(CLAMP(1.000*Y-0.395*(Cr-0.5)-0.581*(Cb-0.5))*255)<<8)  | \
			  ((int)(CLAMP(1.000*Y+2.032*(Cr-0.5)               )*255)))
#elif 0
// YCbCr to RGB for SDTV
#define YUV2RGB(Y,Cb,Cr) (((int)(CLAMP(1.164*Y               +1.596*(Cb-0.5))*255)<<16) | \
                          ((int)(CLAMP(1.164*Y-0.392*(Cr-0.5)-0.813*(Cb-0.5))*255)<<8)  | \
			  ((int)(CLAMP(1.164*Y+2.017*(Cr-0.5)               )*255)))
#elif 0
// Full-range YCbCr to RGB for SDTV
#define YUV2RGB(Y,Cb,Cr) (((int)(CLAMP(1.000*Y               +1.400*(Cb-0.5))*255)<<16) | \
                          ((int)(CLAMP(1.000*Y-0.343*(Cr-0.5)-0.711*(Cb-0.5))*255)<<8)  | \
			  ((int)(CLAMP(1.000*Y+1.765*(Cr-0.5)               )*255)))
#endif

static unsigned int palette[16] = {
#if 0
// http://atariage.com/forums/topic/238672-rgb-color-codes-for-ti-994a-colors/
	0x000000, // transparent
	0x000000, // black
	0x46b83c, // medium green
	0x7ccf70, // light green
	0x5d4fff, // dark blue
	0x7f71ff, // light blue
	0xb66247, // dark red
	0x5cc7ee, // cyan
	0xd86b48, // medium red
	0xfb8f6c, // light red
	0xc3ce42, // dark yellow
	0xd3db77, // light yellow
	0x3ea030, // dark green
	0xb664c6, // magenta
	0xcdcdcd, // gray
	0xffffff, // white
#elif 0
// TMS https://www.smspower.org/maxim/forumstuff/colours.html
	0x000000, // transparent
	0x000000, // black
	0x47b73b, // medium green
	0x7ccf6f, // light green
	0x5d4eff, // dark blue
	0x8072ff, // light blue
	0xb66247, // dark red
	0x5dc8ed, // cyan
	0xd76b48, // medium red
	0xfb8f6c, // light red
	0xc3cd41, // dark yellow
	0xd3da76, // light yellow
	0x3e9f2f, // dark green
	0xb664c7, // magenta
	0xcccccc, // gray
	0xffffff, // white
#elif 0
// SMS https://www.smspower.org/forums/post37531#37531
	0x000000, // transparent
	0x000000, // black
	0x00aa00, // medium green
	0x00ff00, // light green
	0x000055, // dark blue
	0x0000ff, // light blue
	0x550000, // dark red
	0x00ffff, // cyan
	0xaa0000, // medium red
	0xff0000, // light red
	0x555500, // dark yellow
	0xffff00, // light yellow
	0x005500, // dark green
	0xff00ff, // magenta
	0x555555, // gray
	0xffffff, // white
#else
// http://www.unige.ch/medecine/nouspikel/ti99/tms9918a.htm#Colors
// taken from TMS9918A/TMS9928A/TMS9929A Video Display Processors TABLE 2-3 
	0x000000, // transparent
	YUV2RGB(0.00, 0.47, 0.47), // black
	YUV2RGB(0.53, 0.07, 0.20), // medium green
	YUV2RGB(0.67, 0.17, 0.27), // light green
	YUV2RGB(0.40, 0.40, 1.00), // dark blue
	YUV2RGB(0.53, 0.43, 0.93), // light blue
	YUV2RGB(0.47, 0.83, 0.30), // dark red
	YUV2RGB(0.67, 0.00, 0.70), // cyan   NOTE! 992X use Luma .73
	YUV2RGB(0.53, 0.93, 0.27), // medium red
	YUV2RGB(0.67, 0.93, 0.27), // light red
	YUV2RGB(0.73, 0.57, 0.07), // dark yellow
	YUV2RGB(0.80, 0.57, 0.17), // light yellow
	YUV2RGB(0.47, 0.13, 0.23), // dark green
	YUV2RGB(0.53, 0.73, 0.67), // magenta
	YUV2RGB(0.80, 0.47, 0.47), // gray
	YUV2RGB(1.00, 0.47, 0.47), // white
#endif
};


static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *texture = NULL;
static SDL_Texture *dbg_texture = NULL;
static Uint64 ticks_per_frame = 0; // actually ticks per frame << 32
static int first_tick = 0;
static unsigned int frames = 0;

enum {
	PAL_FPS = 50000,
	NTSC_FPS = 59940,
};
void vdp_set_fps(int mfps /* fps*1000 */)
{
	if (mfps == 0) {
		ticks_per_frame = 0; // uncapped framerate
	}
	// This can be used to set frame rate to a more exact fraction
	// Or with another divider to speed up/slow down emulation
	ticks_per_frame = ((Uint64)1000000 << 32) / mfps;
}


void snd_w(unsigned char byte)
{
	// FIXME sound bytes should be written to a buffer with cpu clock counts,
	// so that the register changes can simulated as the audio is generated,
	// TODO isn't there also a CRU bit that controls audio (cassette tape?)
	//fprintf(stderr, "sound %02x\n", byte);
	snd(byte);
}

static void my_audio_callback(void *userdata, Uint8 *stream, int len)
{
	// For AUDIO_U8, len is number of samples to generate
	update(stream, 0, len);
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
		window = SDL_CreateWindow("BuLWiP TI-99/4A",
				SDL_WINDOWPOS_UNDEFINED,
				SDL_WINDOWPOS_UNDEFINED,
				320*2,
				240*2,
				SDL_WINDOW_RESIZABLE);

		renderer = SDL_CreateRenderer(window, -1, 0);

		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); // 0=nearest 1=linear 2=anisotropic
		texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING, 640, 480);
	}

	if (window)
		SDL_PauseAudio(0); // start playing

	if (0) { // print the palette
		int i;
		for (i = 0; i < 16; i++) {
			fprintf(stderr, "%06x\n", palette[i]);
		}
	}
	first_tick = SDL_GetTicks();

	vdp_set_fps(NTSC_FPS);
}



#define TOPBORD 24
#define BOTBORD 24
#define LFTBORD 32
#define RGTBORD 32
#define SPRITES_PER_LINE 4
#define EARLY_CLOCK_BIT 0x80
#define FIFTH_SPRITE 0x40
#define SPRITE_COINC 0x20

static int config_sprites_per_line = SPRITES_PER_LINE;

void vdp_line(unsigned int line,
		unsigned char* restrict reg,
		unsigned char* restrict ram)
{
	SDL_Rect rect = {0, line, 320, 1};
	uint32_t *pixels;
	int pitch = 0;
	uint32_t bg = palette[reg[7] & 0xf];
	uint32_t fg = palette[(reg[7] >> 4) & 0xf];

	palette[0] = bg;

	if (texture) {
		SDL_LockTexture(texture, &rect, (void**)&pixels, &pitch);
	} else {
		static uint32_t *dummy = NULL;
		if (!dummy) dummy = malloc(320*240*sizeof(int));
		pixels = dummy;
	}

	if (line < TOPBORD || line >= TOPBORD+192 || (reg[1] & 0x40) == 0) {
		// draw border or blanking
		for (int i = 0; i < LFTBORD+256+RGTBORD; i++) {
			*pixels++ = bg;
		}
	} else {
		unsigned int sy = line - TOPBORD; // screen y, adjusted for border
		unsigned char *scr = ram + (reg[2] & 0xf) * 0x400;
		unsigned char *col = ram + reg[3] * 0x40;
		unsigned char *pat = ram + (reg[4] & 0x7) * 0x800 + (sy & 7);

		// draw left border
		for (int i = 0; i < LFTBORD; i++) {
			*pixels++ = bg;
		}

		uint32_t *save_pixels = pixels;

		// draw graphics
		unsigned char mode = (reg[0] & 0x02) | (reg[1] & 0x18);

		if (mode == 0x0) {
			// mode 1 (standard)
			scr += (sy / 8) * 32;

			for (unsigned char i = 32; i; i--) {
				unsigned char
					ch = *scr++,
					c = col[ch >> 3];
				unsigned int
					fg = palette[c >> 4],
					bg = palette[c & 15];
				ch = pat[ch * 8];
				for (unsigned char j = 0x80; j; j >>= 1)
					*pixels++ = ch & j ? fg : bg;
			}
		} else if (mode & 0x10) {
			// text mode(s) 
			scr += (sy / 8) * 40;
			for (int i = 0; i < 8; i++) {
				*pixels++ = bg;
			}
			if (mode == 0x10) {
				for (unsigned char i = 40; i; i--) {
					unsigned char ch = *scr++;
					ch = pat[ch * 8];
					for (unsigned char j = 0x80; j != 2; j >>= 1)
						*pixels++ = ch & j ? fg : bg;
				}
			} else if (mode == 0x12) { // text bitmap
				unsigned int patmask = ((reg[4]&3)<<11)|(0x7ff);
				pat = ram + (reg[4] & 0x04) * 0x800 +
					(((sy / 64) * 2048) & patmask) + (sy & 7);
				for (unsigned char i = 40; i; i--) {
					unsigned char ch = *scr++;
					ch = pat[ch * 8];
					for (unsigned char j = 0x80; j != 2; j >>= 1)
						*pixels++ = ch & j ? fg : bg;
				}
			} else { // illegal mode (40 col, 4px fg, 2px bg)
				for (unsigned char i = 40; i; i--) {
					*pixels++ = fg;
					*pixels++ = fg;
					*pixels++ = fg;
					*pixels++ = fg;
					*pixels++ = bg;
					*pixels++ = bg;
				}
			}
			for (int i = 0; i < 8; i++) {
				*pixels++ = bg;
			}

		} else if (mode == 0x02) {
			// mode 2 (bitmap)

			// masks for hybrid modes
			unsigned int colmask = ((reg[3] & 0x7f) << 6) | 0x3f;
			unsigned int patmask = ((reg[4] & 3) << 11) | (colmask & 0x7ff);

			scr += (sy / 8) * 32;  // get row

			col = ram + (reg[3] & 0x80) * 0x40 +
				(((sy / 64) * 2048) & colmask) + (sy & 7);
			pat = ram + (reg[4] & 0x04) * 0x800 +
				(((sy / 64) * 2048) & patmask) + (sy & 7);

			// TODO handle bitmap modes
			for (unsigned char i = 32; i; i--) {
				unsigned char
					ch = *scr++,
					c = col[(ch & patmask) * 8];
				unsigned int
					fg = palette[c >> 4],
					bg = palette[c & 15];
				ch = pat[(ch & colmask) * 8];
				for (unsigned char j = 0x80; j; j >>= 1)
					*pixels++ = ch & j ? fg : bg;
			}
		} else if (mode == 0x08) {
			// multicolor 64x48 pixels
			pat -= (sy & 7); // adjust y offset
			pat += ((sy / 4) & 7);

			scr += (sy / 8) * 32;  // get row

			for (unsigned char i = 32; i; i--) {
				unsigned char
					ch = *scr++,
					c = pat[ch * 8];
				unsigned int
					fg = palette[c >> 4],
					bg = palette[c & 15];
				for (unsigned char j = 0; j < 4; j++)
					*pixels++ = fg;
				for (unsigned char j = 0; j < 4; j++)
					*pixels++ = bg;
			}

		}

		// draw right border
		for (int i = 0; i < RGTBORD; i++) {
			*pixels++ = bg;
		}
		pixels = save_pixels;

		// no sprites in text mode
		if (!(reg[1] & 0x10)) {
			unsigned char sp_size = (reg[1] & 2) ? 16 : 8;
			unsigned char sp_mag = sp_size << (reg[1] & 1); // magnified sprite size
			unsigned char *sl = ram + (reg[5] & 0x7f) * 0x80; // sprite list
			unsigned char *sp = ram + (reg[6] & 0x7) * 0x800; // sprite pattern table
			unsigned char coinc[256] = {};

			// draw sprites  (TODO limit 4 sprites per line, and higher priority sprites)
			struct {
				unsigned char *p;  // Sprite pattern data
				unsigned char x;   // X-coordinate
				unsigned char f;   // Color and early clock bit
			} sprites[SPRITES_PER_LINE]; // TODO Sprite limit hardcoded at 4
			int sprite_count = 0;

			for (unsigned char i = 0; i < 32; i++) {
				unsigned int dy = sy;
				unsigned char y = *sl++; // Y-coordinate minus 1
				unsigned char x = *sl++; // X-coordinate
				unsigned char s = *sl++; // Sprite pattern index
				unsigned char f = *sl++; // Flags and color

				if (y == 0xD0) // Sprite List terminator
					break;
				if (y > 0xD0)
					dy += 256; // wraps around top of screen
				if (y+1+sp_mag <= dy || y+1 > dy)
					continue; // not visible
				if (sp_size == 16)
					s &= 0xfc; // mask sprite index

				//if (f & 15)
				//	printf("%d %x %d %d %d %02x %02x\n", sy, (reg[5]&0x7f)*0x80, sprite_count, y, x, s, f);
				if (sprite_count >= SPRITES_PER_LINE && (reg[8] & FIFTH_SPRITE) == 0) {
					reg[8] &= 0xe0;
					reg[8] |= FIFTH_SPRITE + i;
				}
				if (sprite_count >= config_sprites_per_line)
					break;

				sprites[sprite_count].p = sp + (s * 8) + (dy - (y+1));
				sprites[sprite_count].x = x;
				sprites[sprite_count].f = f;
				sprite_count++;
			}
			// draw in reverse order so that lower sprite index are higher priority
			while (sprite_count > 0) {
				sprite_count--;
				unsigned char *p = sprites[sprite_count].p; // pattern pointer
				int x = sprites[sprite_count].x;
				unsigned char f = sprites[sprite_count].f; // flags and color
				unsigned int c = palette[f & 0xf];
				unsigned int mask = (p[0] << 8) | p[16]; // bit mask of solid pixels
				int count = sp_mag; // number of pixels to draw
				int inc_mask = (reg[1] & 1) ? 1 : 0xff;


				//printf("%d %d %d %04x\n", sprite_count, x, f, mask);
				if (f & EARLY_CLOCK_BIT) {
					x -= 32;
					while (count > 0 && x < 0) {
						if (count & inc_mask)
							mask <<= 1;
						++x;
						count--;
					}
				}

				while (count > 0) {
					if (mask & 0x8000) {
						if (f != 0)  // don't draw transparent color
							pixels[x] = c;
						reg[8] |= coinc[x];
						coinc[x] = SPRITE_COINC;
					}
					if (count & inc_mask)
						mask <<= 1;
					if (++x >= 256)
						break;
					count--;
				}
			}
		}
	}
	if (texture) {
		SDL_UnlockTexture(texture);
	}
}

static unsigned char *text_pat;

void vdp_text_pat(unsigned char *pat)
{
	text_pat = pat;
}

void vdp_text_window(const unsigned char *line, int w, int h, int x, int y, int highlight_line)
{
	char fg_color = 1; //15;
	char bg_color = 14; //4;
	uint32_t bg = palette[highlight_line == 0 ? fg_color : bg_color];
	uint32_t fg = palette[highlight_line == 0 ? bg_color : fg_color];
	uint32_t *pixels;
	int pitch = 0;
	const unsigned char *start = line;

	if (!texture) return;
	SDL_LockTexture(texture, NULL, (void**)&pixels, &pitch);

	pixels += x + y * (pitch/4);
	for (unsigned int j = 0; j < h*8; j++) {
		if ((j&7) == 7) {
			if (j/8 == highlight_line-1) {
				bg = palette[fg_color];  // inverted
				fg = palette[bg_color];
			}
			for (unsigned int i = 0; i < w*6; i++)
				*pixels++ = bg;
			//line += w;
			start = line;
			text_pat -= 7;
			pixels += (pitch/4) - (w*6);

			if (j/8 == highlight_line) {
				bg = palette[bg_color];  // normal
				fg = palette[fg_color];
			}
			continue;
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
			ch = text_pat[ch * 7];
			for (unsigned char m = 0x80; m != 2; m >>= 1)
				*pixels++ = ch & m ? fg : bg;
		}
		if (line[-1] == 0)
			line--;
		else if (line[-1] == '\n' && line[0] == '\r')
			line++;
		else if (line[-1] == '\r' && line[0] == '\n')
			line++;

		pixels += (pitch/4) - (w*6);
		text_pat++;
	}
	SDL_UnlockTexture(texture);
}


extern Uint8 keyboard[8]; // cpu.c
extern void reset(void); // cpu.c
extern int debug_en; // cpu.c
extern int debug_break; // cpu.c

#define SET_KEY(k, val) do { \
		int row = ((k) >> 3) & 7, col = (k) & 7; \
		keyboard[row] = (keyboard[row] & ~(1<<col)) | ((val) << col); \
	} while(0)

//	case 3: //     = . , M N / fire1 fire2
//	case 4: // space L K J H ;
//	case 5: // enter O I U Y P
//	case 6: //       9 8 7 6 0
//	case 7: //  fctn 2 3 4 5 1
//	case 8: // shift S D F G A
//	case 9: //  ctrl W E R T Q
//	case 10://       X C V B Z

enum {  // [7..3]=col bits[2..0]=row
	TI_EQUALS, TI_SPACE, TI_ENTER, TI_FCTN=4, TI_SHIFT, TI_CTRL,
	TI_PERIOD=8, TI_L, TI_O, TI_9, TI_2, TI_S, TI_W, TI_X,
	TI_COMMA,    TI_K, TI_I, TI_8, TI_3, TI_D, TI_E, TI_C,
	TI_M, TI_J, TI_U, TI_7, TI_4, TI_F, TI_R, TI_V,
	TI_N, TI_H, TI_Y, TI_6, TI_5, TI_G, TI_T, TI_B,
	TI_SLASH, TI_SEMICOLON, TI_P, TI_0, TI_1, TI_A, TI_Q, TI_Z,
	TI_FIRE1, TI_LEFT1, TI_RIGHT1, TI_DOWN1, TI_UP1,
	TI_FIRE2=56, TI_LEFT2, TI_RIGHT2, TI_DOWN2, TI_UP2,
	TI_ADDSHIFT = 1<<6,
	TI_NOSHIFT = 1<<7,
	TI_ADDFCTN = 1<<8,
};



int vdp_update(void)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_QUIT) {
			fprintf(stderr, "SDL_QUIT %f fps\n", frames*1000.0/(SDL_GetTicks()-first_tick));
			return -1;
		} else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
			SDL_KeyboardEvent *key = &event.key;
			Uint16 mod = key->keysym.mod;
			int kdn = event.type == SDL_KEYDOWN;
			int k = -1;

			// use sym for virtual key representation (mapped keyboard layout)
			switch (key->keysym.sym) {
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
			case SDLK_l:      k = TI_L; break;
			case SDLK_o:      k = TI_O; break;
			case SDLK_9:      k = TI_9; break;
			case SDLK_2:      k = TI_2; break;
			case SDLK_s:      k = TI_S; break;
			case SDLK_w:      k = TI_W; break;
			case SDLK_x:      k = TI_X; break;

			case SDLK_COMMA:  k = TI_COMMA; break;
			case SDLK_k:      k = TI_K; break;
			case SDLK_i:      k = TI_I; break;
			case SDLK_8:      k = TI_8; break;
			case SDLK_3:      k = TI_3; break;
			case SDLK_d:      k = TI_D; break;
			case SDLK_e:      k = TI_E; break;
			case SDLK_c:      k = TI_C; break;

			case SDLK_m:      k = TI_M; break;
			case SDLK_j:      k = TI_J; break;
			case SDLK_u:      k = TI_U; break;
			case SDLK_7:      k = TI_7; break;
			case SDLK_4:      k = TI_4; break;
			case SDLK_f:      k = TI_F; break;
			case SDLK_r:      k = TI_R; break;
			case SDLK_v:      k = TI_V; break;

			case SDLK_n:      k = TI_N; break;
			case SDLK_h:      k = TI_H; break;
			case SDLK_y:      k = TI_Y; break;
			case SDLK_6:      k = TI_6; break;
			case SDLK_5:      k = TI_5; break;
			case SDLK_g:      k = TI_G; break;
			case SDLK_t:      k = TI_T; break;
			case SDLK_b:      k = TI_B; break;

			case SDLK_SLASH:  k = TI_SLASH; break;
			case SDLK_SEMICOLON: k = TI_SEMICOLON; break;
			case SDLK_p:      k = TI_P; break;
			case SDLK_0:      k = TI_0; break;
			case SDLK_1:      k = TI_1; break;
			case SDLK_a:      k = TI_A; break;
			case SDLK_q:      k = TI_Q; break;
			case SDLK_z:      k = TI_Z; break;

			case SDLK_TAB:    k = TI_FIRE1; break;
			case SDLK_LEFT:   k = TI_LEFT1; break;
			case SDLK_RIGHT:  k = TI_RIGHT1; break;
			case SDLK_DOWN:   k = TI_DOWN1; break;
			case SDLK_UP:     k = TI_UP1; break;

			case SDLK_BACKSPACE: k = TI_S | TI_ADDFCTN; break;
			//case SDLK_DELETE: k = TI_1 | TI_ADDFCTN; break;
			//case SDLK_INSERT: k = TI_2 | TI_ADDFCTN; break;
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

			case SDLK_HOME: if (kdn && (mod & KMOD_CTRL)) debug_en = !debug_en; break;

			case SDLK_F1: if (kdn) debug_break = !debug_break; break;
			case SDLK_F2: if (kdn) debug_break = 2; break;
			case SDLK_F12: if (mod & KMOD_CTRL) reset(); break;
			default: break;
			}
			//fprintf(stderr, "key %d mod %x val %d row %d col=%d\n", key->keysym.scancode, mod, val, row, col);
			if (k != -1) {
				SET_KEY(k, kdn);
				if (k & TI_ADDSHIFT) {
					SET_KEY(TI_SHIFT, kdn || (mod & KMOD_SHIFT));
				}
				if (k & TI_ADDFCTN) {
					SET_KEY(TI_FCTN, kdn || (mod & KMOD_ALT));
					SET_KEY(TI_SHIFT, !kdn && (mod & KMOD_SHIFT));
				}

			}

		}
	}

	if (renderer) {
#if 1
		const SDL_Rect src = {.x = 0, .y = 0, .w = 320, .h = 240};
		SDL_RenderCopy(renderer, texture, debug_en ? NULL : &src, NULL);
#else
		SDL_RenderCopy(renderer, texture, NULL, NULL);
#endif

		// TODO use SDL_GetTicks() to determine actual framerate and 
		// dupe/drop frames to achieve desired VDP freq

		SDL_RenderPresent(renderer);
		if (ticks_per_frame != 0) { // cap frame rate, approximately
			static Uint64 next_time = 0;
			Uint32 now = SDL_GetTicks();
			int time_left = (next_time >> 32) - now;

			if (next_time == 0 || time_left <= 0) {
				next_time = ((Uint64)now << 32);
			} else {
				SDL_Delay(time_left);
			}
			next_time += ticks_per_frame;
		}
		frames++;
		//if (frames == 600) {
		//	fprintf(stderr, "600 frames %f fps\n", frames*1000.0/(SDL_GetTicks()-first_tick));
		//	return -1;
		//}
	}
	return 0;
}

void vdp_done(void)
{
	if (texture) SDL_DestroyTexture(texture);
	if (renderer) SDL_DestroyRenderer(renderer);
	if (window) SDL_CloseAudio();
	SDL_Quit();
}
