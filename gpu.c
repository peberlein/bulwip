/*
 *  gpu.c - TMS 9900 CPU emulation on F18A
 *
 * Copyright (c) 2024 Pete Eberlein
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

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _GNU_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cpu.h"


// TODO maybe move this and drawing code to vdp.c
struct vdp vdp;

#ifdef ENABLE_F18A
// F18A Major/Minor version
#define F18A_VER 0x19

static const struct vdp f18a_defaults = {
	.ram = {},
	.a = 0,
	.latch = 0,
	.reg = {[4] = 1, [48] = 1,
		[VDP_ST+1] = 0xe0,
		[VDP_ST+14] = F18A_VER,
		},
	.y = 0,
	.locked = 1,
	.pal = {
		0x0,0x00, //  0 Transparent
		0x0,0x00, //  1 Black
		0x2,0xC3, //  2 Medium Green
		0x5,0xD6, //  3 Light Green
		0x5,0x4F, //  4 Dark Blue
		0x7,0x6F, //  5 Light Blue
		0xD,0x54, //  6 Dark Red
		0x4,0xEF, //  7 Cyan
		0xF,0x54, //  8 Medium Red
		0xF,0x76, //  9 Light Red
		0xD,0xC3, // 10 Dark Yellow
		0xE,0xD6, // 11 Light Yellow
		0x2,0xB2, // 12 Dark Green
		0xC,0x5C, // 13 Magenta
		0xC,0xCC, // 14 Gray
		0xF,0xFF, // 15 White

		// Palette 1, ECM1 (0 index is always 000) version of palette 0
		0x0,0x00, //  0 Black
		0x2,0xC3, //  1 Medium Green
		0x0,0x00, //  2 Black
		0x5,0x4F, //  3 Dark Blue
		0x0,0x00, //  4 Black
		0xD,0x54, //  5 Dark Red
		0x0,0x00, //  6 Black
		0x4,0xEF, //  7 Cyan
		0x0,0x00, //  8 Black
		0xC,0xCC, //  9 Gray
		0x0,0x00, // 10 Black
		0xD,0xC3, // 11 Dark Yellow
		0x0,0x00, // 12 Black
		0xC,0x5C, // 13 Magenta
		0x0,0x00, // 14 Black
		0xF,0xFF, // 15 White

		// Palette 2, CGA colors
		0x0,0x00, //  0 >000000 (  0   0   0) black
		0x0,0x0A, //  1 >0000AA (  0   0 170) blue
		0x0,0xA0, //  2 >00AA00 (  0 170   0) green
		0x0,0xAA, //  3 >00AAAA (  0 170 170) cyan
		0xA,0x00, //  4 >AA0000 (170   0   0) red
		0xA,0x0A, //  5 >AA00AA (170   0 170) magenta
		0xA,0x50, //  6 >AA5500 (170  85   0) brown
		0xA,0xAA, //  7 >AAAAAA (170 170 170) light gray
		0x5,0x55, //  8 >555555 ( 85  85  85) gray
		0x5,0x5F, //  9 >5555FF ( 85  85 255) light blue
		0x5,0xF5, // 10 >55FF55 ( 85 255  85) light green
		0x5,0xFF, // 11 >55FFFF ( 85 255 255) light cyan
		0xF,0x55, // 12 >FF5555 (255  85  85) light red
		0xF,0x5F, // 13 >FF55FF (255  85 255) light magenta
		0xF,0xF5, // 14 >FFFF55 (255 255  85) yellow
		0xF,0xFF, // 15 >FFFFFF (255 255 255) white

		// Palette 3, ECM1 (0 index is always 000) version of palette 2
		0x0,0x00, //  0 >000000 (  0   0   0) black
		0x5,0x55, //  1 >555555 ( 85  85  85) gray
		0x0,0x00, //  2 >000000 (  0   0   0) black
		0x0,0x0A, //  3 >0000AA (  0   0 170) blue
		0x0,0x00, //  4 >000000 (  0   0   0) black
		0x0,0xA0, //  5 >00AA00 (  0 170   0) green
		0x0,0x00, //  6 >000000 (  0   0   0) black
		0x0,0xAA, //  7 >00AAAA (  0 170 170) cyan
		0x0,0x00, //  8 >000000 (  0   0   0) black
		0xA,0x00, //  9 >AA0000 (170   0   0) red
		0x0,0x00, // 10 >000000 (  0   0   0) black
		0xA,0x0A, // 11 >AA00AA (170   0 170) magenta
		0x0,0x00, // 12 >000000 (  0   0   0) black
		0xA,0x50, // 13 >AA5500 (170  85   0) brown
		0x0,0x00, // 14 >000000 (  0   0   0) black
		0xF,0xFF,  // 15 >FFFFFF (255 255 255) white
	},
};

static int f18a_unlocked(void)
{
	return !vdp.locked;
}
#else
const struct vdp vdp_defaults = {
	.ram = {},
	.a = 0,
	.latch = 0,
	.reg = {0,0,0,0,1,0,0,0,0},
	.y = 0,
};

#endif



void vdp_reset(void)
{
#ifdef ENABLE_F18A
	vdp = f18a_defaults;
#else
	vdp = vdp_defaults;
#endif
}





/****************************************
 * VDP scanline rendering               *
****************************************/


// clamp number between 0.0 and 1.0
#define CLAMP(x) ((x) < 0 ? 0 : (x) > 1 ? 1 : (x))
// take values from 0.0 to 1.0
#if 1
// YUV to RGB for analog TV
#define YUV2RGB(Y,Cb,Cr) (((int)(CLAMP(1.000*Y               +1.140*(Cb-0.5))*255)<<16) | \
                          ((int)(CLAMP(1.000*Y-0.395*(Cr-0.5)-0.581*(Cb-0.5))*255)<<8)  | \
			  ((int)(CLAMP(1.000*Y+2.032*(Cr-0.5)               )*255)))	| \
			  0xff000000
#elif 0
// YCbCr to RGB for SDTV
#define YUV2RGB(Y,Cb,Cr) (((int)(CLAMP(1.164*Y               +1.596*(Cb-0.5))*255)<<16) | \
                          ((int)(CLAMP(1.164*Y-0.392*(Cr-0.5)-0.813*(Cb-0.5))*255)<<8)  | \
			  ((int)(CLAMP(1.164*Y+2.017*(Cr-0.5)               )*255)))	| \
			  0xff000000
#elif 0
// Full-range YCbCr to RGB for SDTV
#define YUV2RGB(Y,Cb,Cr) (((int)(CLAMP(1.000*Y               +1.400*(Cb-0.5))*255)<<16) | \
                          ((int)(CLAMP(1.000*Y-0.343*(Cr-0.5)-0.711*(Cb-0.5))*255)<<8)  | \
			  ((int)(CLAMP(1.000*Y+1.765*(Cr-0.5)               )*255)))	| \
			  0xff000000
#endif

unsigned int palette[16] = {
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



#define TOPBORD 24
#define BOTBORD 24
#define LFTBORD 32
#define RGTBORD 32
#define SPRITES_PER_LINE 4
#define EARLY_CLOCK_BIT 0x80
#define INTERRUPT 0x80
#define FIFTH_SPRITE 0x40
#define SPRITE_COINC 0x20
#ifdef ENABLE_F18A
#define ATTR_PRI 0x80
#define ATTR_FLIP_X 0x40
#define ATTR_FLIP_Y 0x20
#define ATTR_TRANS 0x10
#endif

static int config_sprites_per_line = SPRITES_PER_LINE;


u32 pal_rgb(int idx)
{
#ifdef ENABLE_F18A
	return 0xff000000 | // convert 12-bit RGB to 32-bit ARGB
		((vdp.pal[idx*2] & 0x0f) * 0x110000) | // R
		((vdp.pal[idx*2+1] & 0xf0) * 0x110) |  // G
		((vdp.pal[idx*2+1] & 0x0f) * 0x11);    // B
#else
	return palette[idx];
#endif
}

#ifdef ENABLE_F18A
static void draw_ecm_tiles(
	u8* restrict buf, // row pixels bytes: pxcccccc p=priority c=palette index
		 // must be at least 256+8 bytes   
	u8* restrict reg,
	u8 *ram,

	u16 base_addr,	// Name table base address, 1KB boundaries
	u16 pat_addr,   // pattern table base address, 2KB boundaries
	u16 col_addr,	// color table base address, 64B boundaries
	u16 tpgs,	// tile pattern generate offset size (2048, 1024, 512, 256)
	u8 tps,		// priority and tile palette select in normal mode: P0TT0000
	u8 pos_attr,	// position attributes
	int hto,	// horizontal tile and pixel offset
	int vto,	// vertical tile and pixel offset
	u8 hpsize,	// horizontal page size: 0 = 1 page, 1 = 2 pages
	u8 vpsize	// vertical page size: 0 = 1 page, 1 = 2 pages
	)
{
	unsigned int i, x_off[2] = {0};
	unsigned int scr_rows = reg[49]&0x40 ? 30 : 24;
	//u8 tps = (reg[24]&3) << 4; // Tile 1 palette select in normal mode
	//u8 pos_attr = (reg[50] & 2) >> 1;
	//unsigned int tpgs = 2048 >> ((reg[29] >> 2) & 3); // Tile pattern generator offset size
	int flip_v; // added to pat or col if tile is flipped vertically
	u8 *scr;
	u8 *col = ram + col_addr;
	u8 *pat[3] = {
		ram + (vto & 7) + pat_addr,
		ram + (vto & 7) + ((pat_addr +   tpgs) & 0x3fff),
		ram + (vto & 7) + ((pat_addr + 2*tpgs) & 0x3fff),
	};
	flip_v = 7 - 2*(vto & 7);

	if (hpsize) { // HPSIZE
		if (base_addr & 0x400) { // HPS
			base_addr -= 0x400;
			hto += 256;
		}
		x_off[1] = 1024; // 1K between horizontal scroll pages
	}
	{
		unsigned int y_off = 0;
		if (vpsize) { // VPSIZE
			if (base_addr & 0x800) { // VPS
				base_addr -= 0x800;
				vto += scr_rows * 8;
			}
			y_off += (((vto / 8) / scr_rows) & 1) * (x_off[1] + 1024);
		}
		y_off += ((vto / 8) % scr_rows) * 32;

		scr = ram + base_addr + y_off;
		if (pos_attr)
			col += y_off;
	}

	unsigned int
		start_x = (hto / 8),
		stop_x = ((hto + 255) / 8) + 1;

	switch ((reg[49] >> 4) & 3) { // ECM mode
	case 0: // ECM0 normal
		for (i = start_x; i < stop_x; i++) {
			u8 ch = scr[x_off[(i/32)&1] + (i&31)];
			u8 attr = col[pos_attr ? x_off[(i/32)&1] + (i&31) : ch];
			int flip_y = attr & ATTR_FLIP_Y ? flip_v : 0;
			int flip_x = attr & ATTR_FLIP_X ? 0 : 7;
			u8 p = pat[0][ch*8+flip_y];
			int shift, start = (i-start_x)*8;

			for (shift = 7; shift >= 0; shift--) {
				buf[start+(flip_x ^ shift)] =
					(attr & ATTR_PRI) | tps |
					(p & BIT(shift) ? (attr & 0x0f) : (attr >> 4));
			}
		}
		break;
	case 1: // ECM1-bpp
		tps &= 0x20; // only TPS0
		for (i = start_x; i < stop_x; i++) {
			u8 ch = scr[x_off[(i/32)&1] + (i&31)];
			u8 attr = col[pos_attr ? x_off[(i/32)&1] + (i&31) : ch];
			int flip_y = attr & ATTR_FLIP_Y ? flip_v : 0;
			int flip_x = attr & ATTR_FLIP_X ? 0 : 7;
			u8 p = pat[0][ch*8+flip_y];
			int shift, start = (i-start_x)*8;

			for (shift = 7; shift >= 0; shift--) {
				u8 c = ((p >> shift) & 1);
				if (c || !(attr & ATTR_TRANS)) // not transparent
					buf[start+(flip_x ^ shift)] =
						(attr & ATTR_PRI) | tps | ((attr & 0x0f) << 1) | c;
			}
		}
		break;
	case 2: // ECM2-bpp
		for (i = start_x; i < stop_x; i++) {
			u8 ch = scr[x_off[(i/32)&1] + (i&31)];
			u8 attr = col[pos_attr ? x_off[(i/32)&1] + (i&31) : ch];
			int flip_y = attr & ATTR_FLIP_Y ? flip_v : 0;
			int flip_x = attr & ATTR_FLIP_X ? 0 : 7;
			unsigned int p[2] = {
				pat[0][ch*8+flip_y],
				pat[1][ch*8+flip_y] << 1};
			int shift, start = (i-start_x)*8;

			for (shift = 7; shift >= 0; shift--) {
				u8 c =  ((p[0] >> shift) & 1) |
					((p[1] >> shift) & 2);
				if (c || !(attr & ATTR_TRANS)) // not transparent
					buf[start+(flip_x ^ shift)] =
						(attr & ATTR_PRI) | ((attr & 0x0f) << 2) | c;
			}
		}
		break;
	case 3: // ECM3-bpp
		for (i = start_x; i < stop_x; i++) {
			u8 ch = scr[x_off[(i/32)&1] + (i&31)];
			u8 attr = col[pos_attr ? x_off[(i/32)&1] + (i&31) : ch];
			int flip_y = attr & ATTR_FLIP_Y ? flip_v : 0;
			int flip_x = attr & ATTR_FLIP_X ? 0 : 7;
			unsigned int p[3] = {
				pat[0][ch*8+flip_y],
				pat[1][ch*8+flip_y] << 1,
				pat[2][ch*8+flip_y] << 2};
			int shift, start = (i-start_x)*8;

			for (shift = 7; shift >= 0; shift--) {
				u8 c =  ((p[0] >> shift) & 1) |
					((p[1] >> shift) & 2) |
					((p[2] >> shift) & 4);
				if (c || !(attr & ATTR_TRANS)) // not transparent
					buf[start+(flip_x ^ shift)] =
						(attr & ATTR_PRI) | ((attr & 0x0e) << 2) | c;
			}
		}
		break;
	}

}

static void draw_bitmap(
	u8* restrict buf, // row pixels bytes: pxcccccc p=priority c=palette index
		 // must be at least 256 bytes
	unsigned int sy,  // pixel y offset 0..191
	u8* restrict reg)
{
	unsigned int i, w, h;
	u8 pri_ps, trans, fat, x, y;
	u8 *bmp; // bitmap 2-bits/pixel

	if (!(reg[31]&0x80)) return; // BML_EN

	pri_ps = ((reg[31] & 0x40) << 1) | ((reg[32] & 0x0f) << 2);
	trans = (reg[31] & 0x20) ? 1 : 0;
	fat = (reg[31] & 0x10) ? 1 : 0;

	x = reg[33];
	y = reg[34];
	w = reg[35] ?: 256; // 0 width means 256
	h = reg[36];

	if (sy < y || sy >= y+h) return; // not visible

	bmp = vdp.ram + reg[32] * 64 + (sy-y)*((w+1)/2);

	buf += x;

	int next_mask = fat ? 7 : 3;
	if (trans) {
		for (i = 0; i < w; i++) {
			int shift = 6 - 2 * ((i >> fat) & 3);
			u8 c = ((*bmp) >> shift) & 3;
			if (c) {
				*buf = pri_ps + c;
			}
			buf++;
			if ((i & next_mask) == next_mask) bmp++;
		}
	} else {
		for (i = 0; i < w; i++) {
			int shift = 6 - 2 * ((i >> fat) & 3);
			*buf++ = pri_ps + ((*bmp) >> shift) & 3;
			if ((i & next_mask) == next_mask) bmp++;
		}
	}
}
#endif



static void draw_graphics1_mode(
	u8* restrict buf, // row pixels bytes: pxcccccc p=priority c=palette index
	unsigned int sy,    // screen y: 0..191
	u8 *scr,
	u8* restrict reg,
	u8 *ram)
{
	u8 *col = ram + reg[3] * 0x40;
	u8 *pat = ram + (reg[4] & 0x7) * 0x800 + (sy & 7);

	// mode 1 (standard)
	scr += (sy / 8) * 32;
	for (u8 i = 32; i; i--) {
		u8
			ch = *scr++,
			c = col[ch >> 3],
			fg = c >> 4,
			bg = c & 15;
		ch = pat[ch * 8];
		for (u8 j = 0x80; j; j >>= 1)
			*buf++ = ch & j ? fg : bg;
	}
}

static void draw_text_mode(
	u8* restrict buf, // row pixels bytes: pxcccccc p=priority c=palette index
	int len,  // 640 for 80-col, 320 for 32/40-col
	unsigned int sy,    // screen y: 0..191
	u8 *scr,
	u8* restrict reg,
	u8 *ram,
	int mode)
{
	u8 i = len == 640 ? 80 : 40;
	u8 bg = reg[7] & 0xf;
	u8 fg = (reg[7] >> 4) & 0xf;
	u8 *pat = ram + (reg[4] & 0x7) * 0x800 + (sy & 7);

	if ((mode & MODE_10_TEXT) == 0)
		return;

	// text mode(s)
	scr += (sy / 8) * i;
	//for (int i = 0; i < bord/4; i++) {
	//	*pixels++ = bg;
	//}
	if (mode == MODE_10_TEXT) {
#ifdef ENABLE_F18A
		if (f18a_unlocked() && (reg[50] & 2)) { // POS_ATTR enabled
			u8 *col = ram + reg[3] * 0x40
				+ (sy / 8) * i; // col is attribute table
			for (; i; i--) {
				u8	ch = *scr++,
					attr = *col++;
					fg = attr >> 4,
					bg = attr & 15;
				ch = pat[ch * 8];
				for (u8 j = 0x80; j != 2; j >>= 1)
					*buf++ = ch & j ? fg : bg;
			}
		} else
#endif
		for (; i; i--) {
			u8 ch = *scr++;
			ch = pat[ch * 8];
			for (u8 j = 0x80; j != 2; j >>= 1)
				*buf++ = ch & j ? fg : bg;
		}
	} else if (mode == MODE_12_TEXT_BITMAP) { // text bitmap
		unsigned int patmask = ((reg[4]&3)<<11)|(0x7ff);
		u8 *pat = ram + (reg[4] & 0x04) * 0x800 +
			(((sy / 64) * 2048) & patmask) + (sy & 7);
		for (; i; i--) {
			u8 ch = *scr++;
			ch = pat[ch * 8];
			for (u8 j = 0x80; j != 2; j >>= 1)
				*buf++ = ch & j ? fg : bg;
		}
	} else { // illegal mode (40 col, 4px fg, 2px bg)
		for (; i; i--) {
			*buf++ = fg;
			*buf++ = fg;
			*buf++ = fg;
			*buf++ = fg;
			*buf++ = fg;
			*buf++ = fg;
		}
	}
}


static void draw_graphics2_mode(
	u8* restrict buf, // row pixels bytes: pxcccccc p=priority c=palette index
	unsigned int sy,    // screen y: 0..191
	u8* scr,
	u8* restrict reg,
	u8* ram)
{
	// mode 2 (bitmap)

	// masks for hybrid modes
	unsigned int colmask = ((reg[3] & 0x7f) << 6) | 0x3f;
	unsigned int patmask = ((reg[4] & 3) << 11) | (colmask & 0x7ff);

	scr += (sy / 8) * 32;  // get row

	u8 *col = ram + (reg[3] & 0x80) * 0x40 +
		(((sy / 64) * 2048) & colmask) + (sy & 7);
	u8 *pat = ram + (reg[4] & 0x04) * 0x800 +
		(((sy / 64) * 2048) & patmask) + (sy & 7);

	// handle bitmap mode
	for (u8 i = 32; i; i--) {
		u8
			ch = *scr++,
			c = col[(ch & patmask) * 8],
			fg = c >> 4,
			bg = c & 15;
		ch = pat[(ch & colmask) * 8];
		for (u8 j = 0x80; j; j >>= 1)
			*buf++ = ch & j ? fg : bg;
	}
}


void draw_multicolor_mode(u8* restrict buf,
	unsigned int sy,
	u8 *scr,
	u8 *reg,
	u8 *ram)
{
	// multicolor 64x48 pixels
	//pat -= (sy & 7); // adjust y offset
	//pat += ((sy / 4) & 7);
	u8 *pat = ram + (reg[4] & 0x7) * 0x800 + ((sy / 4) & 7);
	scr += (sy / 8) * 32;  // get row

	for (u8 i = 32; i; i--) {
		u8	ch = *scr++,
			c = pat[ch * 8],
			l = c >> 4,
			r = c & 15;
		for (u8 j = 0; j < 4; j++)
			*buf++ = l;
		for (u8 j = 0; j < 4; j++)
			*buf++ = r;
	}
}

#if 0
	case MODE_SPRITES: {
		// sprite patterns - debug only 
		u8 *sp = ram + (reg[6] & 0x7) * 0x800 // sprite pattern table
			+ (sy & 7);
		scr += (sy / 8) * 32;
		fg = palette[15];
		bg = palette[1];

		for (u8 i = 32; i; i--) {
			u8 ch = *scr++;
			ch = sp[ch * 8];
			for (u8 j = 0x80; j; j >>= 1)
				*pixels++ = ch & j ? fg : bg;
		}
		break; }
	}
}
#endif

void draw_sprites(u8* restrict buf, 
		unsigned int sy,
		u8* restrict reg,
		u8* restrict ram)
{
	u8 sp_size = (reg[1] & 2) ? 16 : 8;
	u8 sp_mag = sp_size << (reg[1] & 1); // magnified sprite size
	u8 *sl = ram + (reg[5] & 0x7f) * 0x80; // sprite list
	u8 *sp = ram + (reg[6] & 0x7) * 0x800; // sprite pattern table
	u8 coinc[256];
	

	// draw sprites  (TODO limit 4 sprites per line, and higher priority sprites)
	struct {
		u8 *p;  // Sprite pattern data
		u8 x;   // X-coordinate
		u8 f;   // Color and early clock bit
#ifdef ENABLE_F18A
		u8 size; // 8 or 16
		u8 mag; // 8 or 16 (unmagnified) 16 or 32 (magnified)
#endif
	} sprites[32]; // TODO Sprite limit hardcoded at 4
	int sprite_count = 0;

	for (u8 i = 0; i < 32; i++) {
		unsigned int dy = sy;
		u8 y = *sl++; // Y-coordinate minus 1
		u8 x = *sl++; // X-coordinate
		u8 s = *sl++; // Sprite pattern index
		u8 f = *sl++; // Flags and color
#ifdef ENABLE_F18A
		
#endif

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
		if ((reg[VDP_ST] & FIFTH_SPRITE) == 0) {
			if (sprite_count == SPRITES_PER_LINE) {
				reg[VDP_ST] &= (INTERRUPT | SPRITE_COINC); // clear existing 5th sprite number
				reg[VDP_ST] |= FIFTH_SPRITE + i;
			}
		}
		if (sprite_count >= config_sprites_per_line)
			break;

		sprites[sprite_count].p = sp + (s * 8) + ((dy - (y+1)) >> (reg[1]&1));
		sprites[sprite_count].x = x;
		sprites[sprite_count].f = f;
		sprite_count++;

		// clear coinc bytes for this sprite
		{
			int n = sp_size;
			if (f & EARLY_CLOCK_BIT) {
				x -= 32;
				if (x < 0) {
					n += x;
					x = 0;
				}
			}
			if (x + n > 256) {
				n = 256 - x;
			}
			if (n > 0) {
				memset(coinc + x, 0, n);
			}
		}
	}
	if ((reg[VDP_ST] & FIFTH_SPRITE) == 0) {
		reg[VDP_ST] &= (INTERRUPT | SPRITE_COINC); // clear existing counter
		reg[VDP_ST] |= sprite_count;
		// this is needed for Miner 2049er to work (tests >02 instead of >20) 
	}

	// draw in reverse order so that lower sprite index are higher priority
	while (sprite_count > 0) {
		sprite_count--;
		u8 *p = sprites[sprite_count].p; // pattern pointer
		int x = sprites[sprite_count].x;
		u8 f = sprites[sprite_count].f; // flags and color
		u8 c = f & 15;
		unsigned int mask = (p[0] << 8) | p[16]; // bit mask of solid pixels
		int count = sp_mag; // number of pixels to draw
		int inc_mask = (reg[1] & 1) ? 1 : 0xff; // only odd pixels : all pixels

		//printf("%d %d %d %04x\n", sprite_count, x, f, mask);
#ifdef ENABLE_F18A
		if (f18a_unlocked()) {
			unsigned int mask2 = 0, mask3 = 0;
			u8 sps = reg[24] & 0x30; // Sprite Palette Select two-MSBs in normal mode
			int spgs = 2048 >> ((reg[29] & 0xc0) >> 6); // Sprite pattern generator offset size
			u8 v_flip = 0; // TODO

			switch (reg[49] & 3) { // ECMS - enhanced color mode, Sprite
			case 3: { // ECM 3-bpp
				u8 *p3 = ram + (((p - ram) + 2*spgs) & 0x3fff);
				mask3 = ((p3[0] << 8) | p3[16]) << 2;
				} // fallthrough
			case 2: { // ECM 2-bpp
				u8 *p2 = ram + (((p - ram) + spgs) & 0x3fff);
				mask2 = ((p2[0] << 8) | p2[16]) << 1;
				} // fallthrough
			case 1: { // ECM 1-bpp
				int shift = 15;
				while (count > 0) {
					u8 c =  ((mask >> shift) & 1) |
						((mask2 >> shift) & 2) |
						((mask3 >> shift) & 4);
					if (c && (buf[x] & ATTR_PRI) == 0) {
						buf[x] = sps | c;
						reg[VDP_ST] |= coinc[x];
						coinc[x] = SPRITE_COINC;
					}
					if (count & inc_mask)
						shift--;
					if (++x >= 256)
						break;
					count--;
				}
				continue; } // next sprite
			case 0:
				c |= sps;
				break;
			}
		}
#endif
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
					buf[x] = c;

				// The first sprite to touch this x-coord will OR
				// a zero in the status reg, but the next sprite to
				// touch the same x-coord will OR the COINC flag.
				reg[VDP_ST] |= coinc[x];
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

void draw_char_patterns(
	u32 *pixels,
	unsigned int sy,    // screen y: 0..191
	u8 *scr,
	u8 mode,
	u8 *reg,
	u8 *ram,
	int bord,
	int len  // 640 for 80-col, 320 for 32/40-col
		// horizontal pixel offset
		// vertical pixel offset
		// reg29 - tile pattern generator offset size, scroll size
		// reg24 - tile palette select in normal and ecm1
	)
{
	u8 buf[640];

	switch (mode) {
	case MODE_1_STANDARD: 
#ifdef ENABLE_F18A
		if (f18a_unlocked()) {
			unsigned int sx = 0;

			//draw_ecm_tiles(buf+bord-(sx&7), sx, sy, reg, scr, ram);
			draw_ecm_tiles(buf, reg, ram,
				(reg[2] & 0x0f) * 0x400, // base address, 1KB boundaries
				reg[4] * 0x800, // Pattern table base address, 2KB boundaries
				reg[3] * 0x40, // Color Table 1 Base Address, 64B boundaries
				2048 >> ((reg[29] >> 2) & 3), // Tile pattern generator offset size
				(reg[24]&3) << 4, // Tile 1 palette select in normal mode
				(reg[50] & 2) >> 1, // Position attributes
				reg[27], // horizontal tile and pixel offset
				sy+reg[28], // vertical tile and pixel offset
				(reg[29] >> 1) & 1, // horizontal page size: 0 = 1 page, 1 = 2 pages
				reg[29] & 1); // vertical page size: 0 = 1 page, 1 = 2 pages


			// TODO Tile layer 2
			// TODO Bitmap layer
			draw_bitmap(buf+bord, sy, reg);
			// TODO ECM Sprites

			break;
		}
#endif
		draw_graphics1_mode(buf+bord, sy, scr, reg, ram);
		break;
	case MODE_2_BITMAP:
		draw_graphics2_mode(buf+bord, sy, scr, reg, ram);
		break;
	case MODE_10_TEXT:
	case MODE_12_TEXT_BITMAP:
		bord += 8; // add 8 pixels since 256-6*40=16
		draw_text_mode(buf+bord, len, sy, scr, reg, ram, mode);
		break;
	case MODE_8_MULTICOLOR:
		draw_multicolor_mode(buf+bord, sy, scr, reg, ram);
		break;
	case MODE_SPRITES: {
		// sprite patterns - debug only
                u8 *sp = ram + (reg[6] & 0x7) * 0x800 // sprite pattern table
                        + (sy & 7);
		u8 *dest = buf + bord;
                scr += (sy / 8) * 32;
                u8 fg = 15;
                u8 bg = 1;

                for (u8 i = 32; i; i--) {
                        u8 ch = *scr++;
                        ch = sp[ch * 8];
                        for (u8 j = 0x80; j; j >>= 1)
                                *dest++ = ch & j ? fg : bg;
                }
		break; }

	}
	for (int i = 0; i < len; i++) {
#ifdef ENABLE_F18A
		if (f18a_unlocked()) {
			for (i = 0; i < len; i++)
				*pixels++ = pal_rgb(buf[i] & 63);
		} else 
#endif
		for (i = 0; i < len; i++)
			*pixels++ = palette[buf[i] & 15];
	}
}


void vdp_line(unsigned int line,
		u8* restrict reg,
		u8* restrict ram)
{
	u8 buf[640]; // enough for 80-col text mode
	int len = 320; // active pixels in buf
	//u32 bg = palette[reg[7] & 0xf];
	//u32 fg = palette[(reg[7] >> 4) & 0xf];
	u8 bg = reg[7] & 0xf;
	u8 fg = (reg[7] >> 4) & 0xf;
	int bord = LFTBORD;
	int top_bord = TOPBORD, bot_bord = TOPBORD+24*8;

	palette[0] = palette[(reg[7] & 0xf) ?: 1];

	if ((reg[0] & 6) == 4 && (reg[1] & 0x18) == 0x10) {
		// 80-col text mode
		len *= 2;
		bord *= 2;
	}

#ifdef ENABLE_F18A
	if (f18a_unlocked() && (reg[49] & 0x40)) {
		top_bord = 0;   // 30-rows mode
		bot_bord = 30*8;
	}
#endif

	memset(buf, bg, len);

	if (line < top_bord || line >= bot_bord || (reg[1] & 0x40) == 0) {
		// draw border or blanking
	} else {
		unsigned int sy = line - top_bord; // screen y, adjusted for border
		u8 *scr = ram + (reg[2] & 0xf) * 0x400;
		u8 mode = (reg[0] & 0x02) | (reg[1] & 0x18);

#ifdef ENABLE_F18A
		if (f18a_unlocked() && (reg[29] & 3)) {
			// scrolling enabled, so page start bits come from vr2
			scr = ram + (reg[2] & 0xc) * 0x400;
		}
#endif

		// draw graphics based on mode
		switch (mode) {
		case MODE_1_STANDARD: 
#ifdef ENABLE_F18A
			if (f18a_unlocked()) {
				unsigned int sx;

				sx = reg[27]; // Horizontal tile and pixel offset
				sy += reg[28]; // Vertical tile and pixel offset

				draw_ecm_tiles(buf+bord-(sx&7), reg, ram,
					(reg[2] & 0x0f) * 0x400, // base address, 1KB boundaries
					reg[4] * 0x800, // Pattern table base address, 2KB boundaries
					reg[3] * 0x40, // Color Table 1 Base Address, 64B boundaries
					2048 >> ((reg[29] >> 2) & 3), // Tile pattern generator offset size
					(reg[24]&3) << 4, // Tile 1 palette select in normal mode
					(reg[50] & 2) >> 1, // Position attributes
					sx, // horizontal tile and pixel offset
					sy, // vertical tile and pixel offset
					(reg[29] >> 1) & 1, // horizontal page size: 0 = 1 page, 1 = 2 pages
					reg[29] & 1); // vertical page size: 0 = 1 page, 1 = 2 pages

				
				// TODO Tile layer 2
				// TODO Bitmap layer
				draw_bitmap(buf+bord, sy, reg);
				// TODO ECM Sprites

				break;
			}
#endif
			draw_graphics1_mode(buf+bord, sy, scr, reg, ram);
			break;
		case MODE_2_BITMAP:
			draw_graphics2_mode(buf+bord, sy, scr, reg, ram);
			break;
		case MODE_10_TEXT:
		case MODE_12_TEXT_BITMAP:
			bord += bord/4; // add 8 pixels since 256-6*40=16
			draw_text_mode(buf+bord, len, sy, scr, reg, ram, mode);
			break;
		case MODE_8_MULTICOLOR:
			draw_multicolor_mode(buf+bord, sy, scr, reg, ram);
			break;
		}

		//draw_char_patterns(pixels, sy, scr, mode, reg, ram, bord, len);
		//pixels += len - 2 * bord;

		// draw borders after patterns to clear scroll gutters
		memset(buf, bg, bord); // left
		memset(buf+len-bord, bg, bord); // right

		// no sprites in text mode
		if (!(reg[1] & 0x10)) {
			draw_sprites(buf+bord, sy, reg, ram);
		}
	}

	{
		u32 *pixels; // pointer to locked texture ram
		int i;
		vdp_lock_texture(line, len, (void**)&pixels);
#ifdef ENABLE_F18A
		if (f18a_unlocked()) {
			for (i = 0; i < len; i++)
				*pixels++ = pal_rgb(buf[i] & 63);
		} else 
#endif
		for (i = 0; i < len; i++)
			*pixels++ = palette[buf[i] & 15];
		vdp_unlock_texture();
	}
}

void vdp_redraw(void)
{
	int y;
	for (y = 0; y < 240; y++) {
		vdp_line(y, vdp.reg, vdp.ram);
	}
}

#if 0
static void print_name_table(u8* reg, u8 *ram)
{
	static const char hex[] = "0123456789ABCDEF";
	u8 *line = ram + (reg[2]&0xf)*0x400;
	int offset = (reg[2]&0xf) == 0 && (reg[4]&0x7) == 0 ?
			0x60 : 0; // use 0 for no offset, 0x60 for XB
	int y, x, w = (reg[1] & 0x10) ? 40 : 32;
	for (y = 0; y < 24; y++) {
		for (x = 0; x < w; x++) {
			u8 c = *line++ - offset;
			printf("%c%c",
				c >= ' ' && c < 127 ? ' ' : hex[(c+offset)>>4],
				c >= ' ' && c < 127 ? c   : hex[(c+offset)&15]);
		}
		printf("\n");
	}
	//printf("Regs: %02x %02x %02x %02x %02x %02x %02x %02x\n",
	//	reg[0],reg[1],reg[2],reg[3],reg[4],reg[5],reg[6],reg[7]);
	// TI BASIC 00 e0 f0 0c f8 86 f8 07
	// XB       00 e0 00 20 00 06 00 07
}
#endif





#ifdef ENABLE_F18A
// GPU program counter, workspace registers
static u16 gPC, wp_regs[16] = {}; // F18A doesn't use workspace pointer
//static const u16 gWP = 0xf000; // FIXME MAYBE
static int cyc = 0;

// reset and load pc
static void gpu_load_pc(void)
{
	gPC = (vdp.reg[54] << 8) | vdp.reg[55];
}

// start execution
static void gpu_trigger(void)
{
	vdp.reg[VDP_ST+2] |= 0x80; // Set GPU status to running
	//printf("GPU trigger PC=%04x\n", gPC);
}

static void gpu_reset(void)
{
	memcpy(vdp.reg, f18a_defaults.reg, sizeof(vdp.reg));
	memcpy(vdp.pal, f18a_defaults.pal, sizeof(vdp.pal));
	vdp.latch = 0;
	vdp.a = 0;
	vdp.locked = 1;
}

static void gpu_write_reg(u8 reg, u8 val)
{
	vdp.reg[reg] = val;
	switch (reg) {
	case 10: break;
	case 11: break;
	case 15: break;
	}
}

// returns 0 if the GPU is running, 1 otherwise (halted by IDLE op)
static int gpu_paused(void)
{
	return vdp.reg[VDP_ST+2] & 0x80 ? 0 : 1;
}
#endif


/******************************************
 * Host accessor functions                *
 ******************************************/

void vdp_write_data(u8 value)
{
#ifdef ENABLE_F18A
	if (!f18a_unlocked()) {
		// original ram write
		vdp.ram[vdp.a] = value;
		vdp.a = (vdp.a + 1) & 0x3fff; // wraps at 16K
		vdp.latch = 0;

	} else if ((vdp.reg[47] & 0x80) == 0) {
		// f18a ram write - use increment
		signed char inc = vdp.reg[48];
		vdp.ram[vdp.a] = value;
		vdp.a = (vdp.a + inc) & 0x3fff; // wraps at 16K
		vdp.latch = 0;

	} else { // DPM mode - update palette registers
		// latch=0 first byte: 0000rrrr
		// latch=1 second byte: ggggbbbb
		vdp.latch ^= 1;
		if (vdp.latch) {
			vdp.pal[(vdp.reg[47] & 63)*2] = value;
		} else {
			vdp.pal[(vdp.reg[47] & 63)*2+1] = value;
			printf("palette[%d]=%03x\n", (vdp.reg[47] & 63),
				(vdp.pal[(vdp.reg[47] & 63)*2]<<8) |
				vdp.pal[(vdp.reg[47] & 63)*2+1]);
			// check auto-increment mode or palette index wrapped
			if ((vdp.reg[47] & 0x40) == 0 || ((++vdp.reg[47]) & 63) == 0) {
				// turn off after wrapping
				vdp.reg[47] = 0;
				vdp.latch = 0;
			}
		}
	}
#else
	vdp.ram[vdp.a] = value;
	vdp.a = (vdp.a + 1) & 0x3fff; // wraps at 16K
	vdp.latch = 0;
#endif
}

static const char* vr_desc[] = {
//       [       |       |       |       |       |       |       |       ]
	"[   0       0       0   |  IE1  |   0   |   M4      M3  | ExtVid]",
	"[4K/16K | BLANK |  IE0  |   M1      M2  |   0   |  SIZE |  MAG  ]",
	"[   0       0       0       0   |NT A0      A1      A2      A3  ]",
	"[CT A0      A1      A2      A3      A4      A5      A6      A7  ]",
	"[   0       0       0       0       0   |PT A0      A1      A2  ]",
	"[   0   |SAT A0     A1      A2      A3      A4      A5      A6  ]",
	"[   0       0       0       0       0   |SP A0      A1      A2  ]",
	"[  FG0     FG1     FG2     FG3  |  BG0     BG1     BG2     BG3  ]",
	"[7                                                              ]",
	"[8                                                              ]",
	"[9                                                              ]",
	"[   0       0       0       0   |NT2 A0     A1      A2      A3  ]",
	"[CT2 A0     A1      A2      A3      A4      A5      A6      A7  ]",
	"[12                                                             ]",
	"[13                                                             ]",
	"[14                                                             ]",
	"[   0   |CNT_RST|CNT_SNP|CNT_ST |   S0      S1      S2      S3  ]",
	"[16                                                             ]",
	"[17                                                             ]",
	"[18                                                             ]",
	"[  IL0     IL1     IL2     IL3     IL4     IL5     IL6     IL7  ]",
	"[20                                                             ]",
	"[21                                                             ]",
	"[22                                                             ]",
	"[23                                                             ]",
	"[   0       0   |  SPS0    SPS1 | T2PS0   T2PS1 | T1PS0   T1PS1 ]",
	"[  HTO0    HTO1    HTO2    HTO3    HTO4 |  HPO0    HPO1    HPO2 ]",
	"[  VTO0    VTO1    VTO2    VTO3    VTO4 |  VPO0    VPO1    VPO2 ]",
	"[  HTO0    HTO1    HTO2    HTO3    HTO4 |  HPO0    HPO1    HPO2 ]",
	"[  VTO0    VTO1    VTO2    VTO3    VTO4 |  VPO0    VPO1    VPO2 ]",
	"[ SPGS0   SPGS1 |HPSIZE2|VPSIZE2| TPGS0   TPGS1 |HPSIZE1 VPSIZE1]",
	"[   0       0       0   | SPMAX0  SPMAX1  SPMAX2  SPMAX3  SPMAX4]",
	"[ BML_EN|BML_PRI|BML_TRA|BML_FAT| BMLPS0  BMLPS1  BMLPS2  BMLPS3]",
	"[ BMLBA0  BMLBA1  BMLBA2  BMLBA3  BMLBA4  BMLBA5  BMLBA6  BMLBA7]",
	"[ BMLX0   BMLX1   BMLX2   BMLX3   BMLX4   BMLX5   BMLX6   BMLX7 ]",
	"[ BMLY0   BMLY1   BMLY2   BMLY3   BMLY4   BMLY5   BMLY6   BMLY7 ]",
	"[ BMLW0   BMLW1   BMLW2   BMLW3   BMLW4   BMLW5   BMLW6   BMLW7 ]",
	"[ BMLH0   BMLH1   BMLH2   BMLH3   BMLH4   BMLH5   BMLH6   BMLH7 ]",
	"[37                                                             ]",
	"[38                                                             ]",
	"[39                                                             ]",
	"[40                                                             ]",
	"[41                                                             ]",
	"[42                                                             ]",
	"[43                                                             ]",
	"[44                                                             ]",
	"[45                                                             ]",
	"[46                                                             ]",
	"[  DPM  |AUTOINC|  PR0     PR1     PR2     PR3     PR4     PR5  ]",
	"[SIGNBIT|  INC0    INC1    INC2    INC3    INC4    INC5    INC6 ]",
	"[VRRESET|GPUHTRG|GPUVTRG|TL1_OFF| RPTMAX|SCANLNS|POSATTR|TIL2PRI]",
	"[   0       0   |STPSPR0 STPSPR1 STPSPR2 STPSPR3 STPSPR4 STPSPR5]",
	"[52                                                             ]",
	"[53                                                             ]",
	"[ GPU0    GPU1    GPU2    GPU3    GPU4    GPU5    GPU6    GPU7  ]",
	"[ GPU8    GPU9    GPU10   GPU11   GPU12   GPU13   GPU14   GPU15 ]",
	"[   0       0       0       0       0       0       0   | GPU OP]",
	"[   0       0       0       1       1       1   |   0       0   ]",
	"[   0       0       0       0  |  GCLK0   GCLK1   GCLK2   GCLK3 ]",
	"[59                                                             ]",
	"[60                                                             ]",
	"[61                                                             ]",
	"[62                                                             ]",
	"[63                                                             ]",
};

void vdp_write_addr(u8 value)
{
	vdp.latch ^= 1;
	if (vdp.latch) {
		// first low byte 
		vdp.a = value;

	// else second high byte: register / address / read
	} else if (value & 0x80) {

		// register write
		undo_push(UNDO_VDPR, (value & 0x7) | vdp.reg[value & 7]);
		//fprintf(stderr, "VDP[%02x]=%02x %s\n", value&0x7f, vdp.a & 0xff, vr_desc[value&63]);
#ifdef ENABLE_F18A
		u8 r = value & 0x7f;
		if (f18a_unlocked()) {
			if (r >= 64) goto bad_vreg;
			// unlocked access to all 64 regs
			gpu_write_reg(r, vdp.a & 0xff);
			if (r == 50 && (vdp.a & 0x80)) {  // check for VR_RESET
				gpu_reset();
				
			} else if (r == 55) { // GPU LSB address
				if (gpu_paused()) {
					gpu_load_pc();
					gpu_trigger();
				}
			} else if (r == 56) { // GPU operation
				if (gpu_paused()) {
					if (vdp.a & 1) { // trigger execution
						gpu_trigger();
					} else { // reset and load PC
						gpu_load_pc();
					}
				}
			}
		} else if (r < 8 || (vdp.reg[0] & 0x6) != 4) {
			// masked access to original 8 regs, unless 80-col mode is set
			vdp.reg[r & 7] = vdp.a & 0xff;
		} else { bad_vreg:
			// ignore writes to other VRs
			fprintf(stderr, "Attempted VR%02x = %02x\n", r, vdp.a & 0xff);
		}
		if (r == 0x39) {
			// to unlock F18A, write 0x1c twice to reg 57 (0x39)
			if (!f18a_unlocked() && (vdp.a & 0xff) != 0x1c) {
				vdp.locked = 1; // re-locked
			} else if (vdp.locked == 2) {
				vdp.locked = 0; // unlocked
				fprintf(stderr, "F18A unlocked\n");
			} else {
				vdp.locked = 2; // half-unlocked
			}
		}
#else
		vdp.reg[value & 7] = vdp.a & 0xff;
#endif

#ifdef TRACE_VDP
		fprintf(stdout, "VDP register %X %02X at PC=%04X\n", value & 7, vdp.a & 0xff, get_pc());
#endif
	} else if (value & 0x40) {
		// set address for data write
		vdp.a |= (value & 0x3f) << 8;
		//debug_log("VDP address for write %04X\n", vdp.a);
	} else {
		// set address for data read
		vdp.a |= (value & 0x3f) << 8;
		// FIXME: this should start reading the data and increment the address
		//debug_log("VDP address for read %04X at PC=%04X\n", vdp.a, get_pc());
	}
}


u8 vdp_read_status_reg(int idx)
{
	//printf("%s: %d\n", __func__, idx);
#ifdef ENABLE_F18A
	if (!f18a_unlocked())
		return vdp.reg[VDP_ST];
	switch (idx) {
	case 0: return vdp.reg[VDP_ST];
	case 1: return 0xe0 + 2*(vdp.y >= 240) + (vdp.y == vdp.reg[19] && vdp.reg[19] != 0);
	case 2: return vdp.reg[VDP_ST+2];
	case 3: return vdp.y;
	case 14: return F18A_VER;
	case 15: return vdp.ram[vdp.a & 0x3fff];
	default: printf("SR%d not implemented\n", idx); return 0;
	}
#else
	return vdp.reg[VDP_ST];
#endif
}

u8 vdp_read_status(void)
{
#ifdef ENABLE_F18A
	u8 st = f18a_unlocked() ? vdp.reg[15] & 15 : 0;
	u8 value = vdp_read_status_reg(st);
	//debug_log("VDP_STATUS=%0x\n", vdp.reg[VDP_ST]);
	//undo_push(UNDO_VDPST, vdp.reg[VDP_ST]);
	if (st == 0) {
		vdp.reg[VDP_ST] &= ~(INTERRUPT | FIFTH_SPRITE | SPRITE_COINC); // clear interrupt flags
		interrupt(-1); // deassert INTREQ
	}
#else
	u8 value = vdp.reg[VDP_ST];
	//debug_log("VDP_STATUS=%0x\n", vdp.reg[VDP_ST]);
	undo_push(UNDO_VDPST, vdp.reg[VDP_ST]);
	vdp.reg[VDP_ST] &= ~(INTERRUPT | FIFTH_SPRITE | SPRITE_COINC); // clear interrupt flags
	interrupt(-1); // deassert INTREQ
#endif
	return value;
}

u8 vdp_read_status_safe(void) // no clearing flags
{
#ifdef ENABLE_F18A
	u8 st = f18a_unlocked() ? vdp.reg[15] & 15 : 0;
	return vdp.reg[VDP_ST+st];
#else
	return vdp.reg[VDP_ST];
#endif
}

u8 vdp_read_data(void)
{
	return vdp.ram[vdp.a++ & 0x3fff];
	//vdp.a = (vdp.a+1) & 0x3fff;
}

u8 vdp_read_data_safe(void) // no post_increment
{
	return vdp.ram[vdp.a & 0x3fff];
}


#ifdef ENABLE_F18A

// 150000000 gpu cycles per second
// 59.94 frames per second (59.922743404)
// pixel clock (5369317.5 Hz)
// frame time 16.6681545 ms
// line time 63.695246 us
// 262 lines per frame (342 pixels/line)
// 9551.536269093521 gpu cycles per line
#define CYCLES_PER_LINE 9552


/*
New or modified instructions for the F18A 9900-based GPU

Inst Opcode Addressing               New name                
CALL >0C80  0000 1100 10Ts SSSS  new CALL     Call subroutine, push return address on stack (stack pointer is R15)
RET  >0C00  0000 1100 0000 0000  new RET      Return from subroutine, pop return address from stack (stack pointer is R15)
PUSH >0D00  0000 1101 00Ts SSSS  new PUSH     Push a 16-bit word onto the stack
POP  >0F00  0000 1111 00Td DDDD  new POP      Pop a 16-bit word off of the stack
SLC  >0E00  0000 1110 00Ts SSSS  new SLC      Shift Left Circular
CKON >03A0                       mod SPI_EN   Sets the chip enable line to the SPI Flash ROM low (enables the ROM)
CKOF >03C0                       mod SPI_DS   Sets the chip enable line to the SPI Flash ROM high (disables the ROM)
IDLE >0340                       mod IDLE     Forces the GPU state machine to the idle state, restart with VR56 trigger
LDCR >3000                       mod SPI_OUT  Writes a byte (always a byte operation) to the SPI Flash ROM
STCR >3400                       mod SPI_IN   Reads a byte (always a byte operation) from the SPI Flash ROM
RTWP >0380                       mod RTWP     Does not use R13, only performs R14->PC, R15->status flags
XOP  >2C00                       mod PIX      New dedicated pixel plotting and addressing instruction

Unimplemented instructions
SBO
SBZ
TB
BLWP
STWP
LWPI
LIMI
RSET
LREX
*/





/******************************************
 * Memory accessor functions              *
 ******************************************/

/*
VRAM 14-bit, 16K @ >0000 to >3FFF (0011 1111 1111 1111)
GRAM 11-bit, 2K  @ >4000 to >47FF (0100 x111 1111 1111)
PRAM  7-bit, 128 @ >5000 to >5x7F (0101 xxxx x111 1111)
VREG  6-bit, 64  @ >6000 to >6x3F (0110 xxxx xx11 1111)
current scanline @ >7000 to >7xx0 (0111 xxxx xxxx xxx0)
blanking         @ >7001 to >7xx1 (0111 xxxx xxxx xxx1)
32-bit counter   @ >8000 to >8xx6 (1000 xxxx xxxx x110)
32-bit rng       @ >9000 to >9xx6 (1001 xxxx xxxx x110)
F18A version     @ >A000 to >Axxx (1010 xxxx xxxx xxxx)
GPU status data  @ >B000 to >Bxxx (1011 xxxx xxxx xxxx)
workspace regs   @ >F000 TODO

*/


#define always_inline inline __attribute((always_inline))

// These may be overridden by debugger for breakpoints/watchpoints
//static u16 (*iaq_func)(u16); // instruction acquisition

static u16 mem_r(u16 address)
{
	address &= ~1; // word aligned
	if (address <= 0x47FF) // VRAM
		return (vdp.ram[address] << 8) | vdp.ram[address+1];
	switch (address >> 12) {
	case 5: // PRAM
		address &= 0x7f; return (vdp.pal[address] << 8) | vdp.pal[address+1];
	case 6: // VREG
		address &= 0x3f; return (vdp.reg[address] << 8) | vdp.reg[address+1];
	case 7: // 0=current scanline 1=blanking
		if (vdp.y < 192) return vdp.y << 8; // scanline=y blanking=0
		return 1; // scanline=0 blanking=1
	case 0xa: // F18A version
		return F18A_VER;
	case 0xb: // GPU status
		address &= 0x0f;
		return (vdp_read_status_reg(address) << 8) |
			vdp_read_status_reg(address+1);
	//case 0xf: // workspace regs
	//	return wp_regs[(address >> 1) & 15];
	}
	return 0xffff;
}


static void mem_w(u16 address, u16 value)
{
	u8 hi = value >> 8, lo = value & 0xff;
	//printf("vdp_mem_w: %04x %04x\n", address, value);
	address &= ~1; // word aligned
	if (address <= 0x47FF) { // VRAM
		vdp.ram[address] = hi;
		vdp.ram[address+1] = lo;
		return;
	}
	switch (address >> 12) {
	case 5: // PRAM
		address &= 0x7f;
		vdp.pal[address] = hi;
		vdp.pal[address+1] = lo;
		break;
	case 6: // VREG
		address &= 0x3f;
		gpu_write_reg(address, hi);
		gpu_write_reg(address+1, lo);
		//vdp.reg[address] = hi;
		//vdp.reg[address+1] = lo;
		break;
	case 0xb:
		// status regs not writable?
		break;
	//case 0xf: // workspace regs
	//	wp_regs[(address >> 1) & 15] = value;
	//	break;
	}
}

static always_inline u16 reg_r(u8 reg)
{
	return wp_regs[reg];
}

static always_inline void reg_w(u8 reg, u16 value)
{
	wp_regs[reg] = value;
}





/****************************************
 * Status flags                         *
 ****************************************/

enum {
	ST_LGT = 0x80, // L> Logical greater than
	ST_AGT = 0x40, // A> Arithmetic greater than
	ST_EQ  = 0x20, // Equal
	ST_C   = 0x10, // Carry
	ST_OV  = 0x08, // Overflow
	ST_OP  = 0x04, // Odd parity
	ST_X   = 0x02, // Extended operation
	ST_IM  = 0x000f, // Interrupt mask
};

static u8 st_flg = 0; // status register flags
static u8 st_int = 0; // status register interrupt mask


static u16 get_g_st(void) { return ((u16)st_flg << 8) | st_int; }
static void set_g_st(u16 new_st) { st_flg = new_st >> 8; st_int = new_st & ST_IM; }
static always_inline void set_C (void) { st_flg |= ST_C; }
static always_inline void clr_C (void) { st_flg &= ~ST_C; }
static always_inline void set_OV(void) { st_flg |= ST_OV; }
static always_inline void clr_OV(void) { st_flg &= ~ST_OV; }
static always_inline void set_OP(void) { st_flg |= ST_OP; }
static always_inline void clr_OP(void) { st_flg &= ~ST_OP; }
static always_inline void set_X (void) { st_flg |= ST_X; }
static always_inline void set_EQ(void) { st_flg |= ST_EQ; }
static always_inline void clr_EQ(void) { st_flg &= ~ST_EQ; }
static always_inline void set_IM(u8 i) { st_int = i & ST_IM; }

static always_inline int tst_LT(void) { return !(st_flg & (ST_AGT|ST_EQ)); }
static always_inline int tst_LE(void) { return !(st_flg & ST_LGT) || (st_flg & ST_EQ); }
static always_inline int tst_EQ(void) { return st_flg & ST_EQ; }
static always_inline int tst_HE(void) { return st_flg & (ST_LGT|ST_EQ); }
static always_inline int tst_GT(void) { return st_flg & ST_AGT; }
static always_inline int tst_C (void) { return st_flg & ST_C; }
static always_inline int tst_OV(void) { return st_flg & ST_OV; }
static always_inline int tst_L (void) { return !(st_flg & (ST_LGT|ST_EQ)); }
static always_inline int tst_H (void) { return (st_flg & ST_LGT) && !(st_flg & ST_EQ); }
static always_inline int tst_OP(void) { return st_flg & ST_OP; }

static always_inline u16 status_parity(u16 a)
{
	if (__builtin_parity(a)) set_OP(); else clr_OP();
	return a;
}

static always_inline void status_equal(u16 a, u16 b)
{
	if (a == b) st_flg |= ST_EQ; else st_flg &= ~ST_EQ;
}

static always_inline void status_arith(u16 a, u16 b)
{
	if (a == b) {
		st_flg = (st_flg | ST_EQ) & ~(ST_LGT | ST_AGT);
	} else {
		st_flg &= ~(ST_LGT | ST_AGT | ST_EQ);
		if (a > b) st_flg |= ST_LGT;
		if ((s16)a > (s16)b) st_flg |= ST_AGT;
	}
}

//#define USE_ZERO_TABLE
#ifdef USE_ZERO_TABLE
// status bits for x compared to zero
#define ST0(x) ((((x)==0) * ST_EQ) | (((x)>0) * ST_LGT) | (((s16)(x)>(s16)0) * ST_AGT))
#define STa(x) ST0(x),ST0(x+1     ),ST0(x+2     ),ST0(x+3     ),ST0(x+4     ),ST0(x+5     ),ST0(x+6     ),ST0(x+7     )
#define STb(x) STa(x),STa(x+0x8   ),STa(x+0x10  ),STa(x+0x18  ),STa(x+0x20  ),STa(x+0x28  ),STa(x+0x30  ),STa(x+0x38  )
#define STc(x) STb(x),STb(x+0x40  ),STb(x+0x80  ),STb(x+0xc0  ),STb(x+0x100 ),STb(x+0x140 ),STb(x+0x180 ),STb(x+0x1c0 )
#define STd(x) STc(x),STc(x+0x200 ),STc(x+0x400 ),STc(x+0x600 ),STc(x+0x800 ),STc(x+0xa00 ),STc(x+0xc00 ),STc(x+0xe00 )
#define STe(x) STd(x),STd(x+0x1000),STd(x+0x2000),STd(x+0x3000),STd(x+0x4000),STd(x+0x5000),STd(x+0x6000),STd(x+0x7000)

const static u8 zero_table[0x10000] = {STe(0),STe(0x8000)};
#endif

// calculate LGT, AGT, EQ flags compared to zero
static always_inline u16 status_zero(u16 a)
{
#ifdef USE_ZERO_TABLE
	st_flg = (st_flg & ~(ST_EQ|ST_LGT|ST_AGT)) | (zero_table[a]);
#else
	status_arith(a, 0);
#endif
	return a;
}


/****************************************
 * Helper functions                     *
 ****************************************/

static always_inline u16 swpb(u16 x)
{
	return (x << 8) | (x >> 8);
}

static always_inline u16 add(u16 a, u16 b)
{
	u16 res;
	// unsigned overflow is carry
	if (__builtin_add_overflow(a, b, &res))
		set_C(); else clr_C();
	// signed overflow is overflow
	if (__builtin_add_overflow((s16)a, (s16)b, (s16*)&res))
		set_OV(); else clr_OV();
	return status_zero(res);
}

static always_inline u16 sub(u16 a, u16 b)
{
	u16 res;
	// unsigned overflow is carry
	//if (__builtin_sub_overflow(a, b, &res))
	if (a >= b)
		set_C(); else clr_C();
	// signed overflow is overflow
	if (__builtin_sub_overflow((s16)a, (s16)b, (s16*)&res))
		set_OV(); else clr_OV();
	return status_zero(res);
}


static always_inline int shift_count(u16 op)
{
	int count = (op >> 4) & 15 ?: reg_r(0) & 15 ?: 16;
	cyc += 2 * count;
	return count;
}



/*************************************************
 * Ts source and Td destination operand decoding *
 *************************************************/

// source returned as value (indirects are followed)
static always_inline u16 Ts(u16 op, u16 *pc)
{
	u16 val, addr = 0;
	u16 reg = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		val = reg_r(reg);
		break;
	case 1: // *Rx
		val = mem_r(reg_r(reg));
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		if (reg) addr = reg_r(reg); else cyc += 2;
		addr += mem_r(*pc);
		*pc += 2;
		val = mem_r(addr);
		cyc += 4;
		break;
	case 3: // *Rx+
		addr = reg_r(reg);
		val = mem_r(addr);   // read value, then update
		reg_w(reg, addr + 2);
		cyc += 4;
		break;
	}
	return val;
}

// source returned as byte value (in high byte, low byte is zero)
// (indirects are followed)
static always_inline u16 TsB(u16 op, u16 *pc)
{
	u16 val, addr = 0;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx   2c
		return reg_r(reg) & 0xff00;
	case 1: // *Rx  6c
		addr = reg_r(reg);
		val = mem_r(addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0  10c
		if (reg) addr = reg_r(reg); else cyc += 2;
		addr += mem_r(*pc);
		*pc += 2;
		val = mem_r(addr);
		cyc += 4;
		break;
	case 3: // *Rx+   10c
		addr = reg_r(reg);
		val = mem_r(addr);
		reg_w(reg, addr + 1);
		cyc += 2;
		break;
	}
	return (addr & 1) ? val << 8 : val & 0xff00;
}

struct val_addr {
	u16 val, addr;
};

static always_inline struct val_addr Td(u16 op, u16 *pc)
{
	struct val_addr va;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx  2c
		va.val = reg_r(reg);
		va.addr = 0; //gWP + 2 * reg;
		break;
	case 1: // *Rx  6c
		va.addr = reg_r(reg);
		va.val = mem_r(va.addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0  10c
		va.addr = 0;
		if (reg) va.addr = reg_r(reg); else cyc += 2;
		va.addr += mem_r(*pc);
		*pc += 2;
		va.val = mem_r(va.addr);
		cyc += 4;
		break;
	case 3: // *Rx+   10c
		va.addr = reg_r(reg);
		va.val = mem_r(va.addr);
		//reg_w(reg, va.addr + 2); // Post-increment must be done later
		cyc += 4;
		break;
	}
	return va;
}

static always_inline struct val_addr TdB(u16 op, u16 *pc)
{
	struct val_addr va;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx
		va.val = reg_r(reg);
		va.addr = 0; //gWP + 2 * reg;
		break;
	case 1: // *Rx
		va.addr = reg_r(reg);
		va.val = mem_r(va.addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		va.addr = 0;
		if (reg) va.addr = reg_r(reg); else cyc += 2;
		va.addr += mem_r(*pc);
		*pc += 2;
		va.val = mem_r(va.addr);
		cyc += 4;
		break;
	case 3: // *Rx+
		va.addr = reg_r(reg);
		va.val = mem_r(va.addr);
		//reg_w(reg, va.addr + 1); // Post-increment must be done later
		cyc += 2;
		break;
	}
	return va;
}

static always_inline void Td_post_increment(u16 op, struct val_addr va, int bytes)
{
	if (((op >> 4) & 3) == 3) {
		// post increment
		reg_w(op & 15, va.addr + bytes);
	}
}

static always_inline void mem_w_Td(u16 op, struct val_addr va)
{
	if ((op >> 3) == 0)
		reg_w(op & 15, va.val);
	else
		mem_w(va.addr, va.val);
	Td_post_increment(op, va, 2);
}

static always_inline void mem_w_TdB(u16 op, struct val_addr va)
{
	if ((op >> 3) == 0)
		reg_w(op & 15, va.val);
	else
		mem_w(va.addr, va.val);
	Td_post_increment(op, va, 1);
}

static always_inline void byte_op(u16 *op, u16 *pc, u16 *ts, struct val_addr *td)
{
	*ts = TsB(*op, pc);
	*op >>= 6;
	*td = TdB(*op, pc);
}

static always_inline void word_op(u16 *op, u16 *pc, u16 *ts, struct val_addr *td)
{
	*ts = Ts(*op, pc);
	*op >>= 6;
	*td = Td(*op, pc);
}


/****************************************
 * Instruction execution                *
 ****************************************/


// This maps instruction opcodes from 16 bits to 7 bits, making switch lookup table more efficient
#define DECODE(x) ((((x) >> (24-__builtin_clz(x))) & 0x78) | (22-__builtin_clz(x)))

// instruction opcode decoding using count-leading-zeroes (clz)

static int gpu_disasm(u16 pc);
static char gpu_text[256];

void gpu(void)
{
	u16 op, pc = gPC;
	u16 ts;
	struct val_addr td;

	if (gpu_paused()) { // GPU not executing
		if ((vdp.reg[50] & 0x40) && vdp.y < 240 ) // GPU_HTRIG
			gpu_trigger();
		else if ((vdp.reg[50] & 0x20) && vdp.y == 246) // GPU_VTRIG
			gpu_trigger();
		else
			return;  // GPU not executing
	}

	cyc = -CYCLES_PER_LINE;

decode_op:
	// when cycle counter rolls positive, go out and render a scanline
	if (cyc > 0)
		goto done;
decode_op_now:

	op = mem_r(pc);
	// if this mem read triggers a breakpoint, it will save the cycles
	// before reading the memory and will return C99_BRK
	pc += 2;
execute_op:
	cyc += 6; // base cycles

	if (0) {
		gpu_disasm(pc-2);
		printf("%s", gpu_text);
	}

	switch (DECODE(op)) {
	LI:   case DECODE(0x0200): reg_w(op&15, status_zero(mem_r(pc))); pc += 2; goto decode_op;
	AI:   case DECODE(0x0220): reg_w(op&15, add(reg_r(op&15), mem_r(pc))); pc += 2; goto decode_op;
	ANDI: case DECODE(0x0240): reg_w(op&15, status_zero(reg_r(op&15) & mem_r(pc))); pc += 2; goto decode_op;
	ORI:  case DECODE(0x0260): reg_w(op&15, status_zero(reg_r(op&15) | mem_r(pc))); pc += 2; goto decode_op;
	CI:   case DECODE(0x0280): status_arith(reg_r(op&15), mem_r(pc)); cyc += 2; pc += 2; goto decode_op;
	//STWP: case DECODE(0x02A0): reg_w(op&15); goto decode_op;
	STST: case DECODE(0x02C0): reg_w(op&15, get_g_st()); goto decode_op;
	//LWPI: case DECODE(0x02E0): cyc -= 2; /*wp = mem_r(pc);*/ cyc += 2; pc += 2; goto decode_op;
	//LIMI: case DECODE(0x0300): cyc -= 2; set_IM(mem_r(pc) & 15); pc += 2; gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; goto decode_op;

	// IDLE >0340                       mod IDLE     Forces the GPU state machine to the idle state, restart with VR56 trigger
	IDLE: case DECODE(0x0340): vdp.reg[VDP_ST+2] &= ~0x80; goto done;
	//RSET: case DECODE(0x0360): debug_log("RSET not implemented\n"); /* TODO */ goto decode_op;
	// RTWP >0380                       mod RTWP     Does not use R13, only performs R14->PC, R15->status flags
	RTWP: case DECODE(0x0380): set_g_st(reg_r(15)); pc = reg_r(14); goto decode_op;
	// CKON >03A0                       mod SPI_EN   Sets the chip enable line to the SPI Flash ROM low (enables the ROM)
	CKON: case DECODE(0x03A0): debug_log("CKON/SPI_EN not implemented\n");/* TODO */ goto decode_op;
	// CKOF >03C0                       mod SPI_DS   Sets the chip enable line to the SPI Flash ROM high (disables the ROM)
	CKOF: case DECODE(0x03C0): debug_log("CKOF/SPI_DS not implemented\n");/* TODO */ goto decode_op;
	//LREX: case DECODE(0x03E0): debug_log("LREX not implemented\n");/* TODO */ goto decode_op;
#if 0
	BLWP: case DECODE(0x0400):
		cyc += 8;
		td = Td(op, &pc);
		//debug_log("OLD pc=%04X wp=%04X st=%04X  NEW pc=%04X wp=%04X\n", pc, st, safe_r(va.addr+2), va.val);
		mem_w(td.val + 2*13);
		mem_w(td.val + 2*14, pc);
		mem_w(td.val + 2*15, get_g_st());
		pc = mem_r(td.addr + 2);
		wp = td.val;
		Td_post_increment(op, td, 2);
		goto decode_op_now; // next instruction cannot be interrupted
#endif
	B:    case DECODE(0x0440): cyc -= 2; td = Td(op, &pc); Td_post_increment(op, td, 2); pc = td.addr; goto decode_op;
	X:    case DECODE(0x0480): cyc -= 2; td = Td(op, &pc); Td_post_increment(op, td, 2); op = td.val; /*printf("X op=%x pc=%x\n", op, pc);*/ goto execute_op;
	CLR:  case DECODE(0x04C0): cyc -= 2; td = Td(op, &pc); td.val = 0; mem_w_Td(op, td); goto decode_op;
	NEG:  case DECODE(0x0500): cyc -= 2; td = Td(op, &pc); td.val = sub(0, td.val); mem_w_Td(op, td); goto decode_op;
	INV:  case DECODE(0x0540): cyc -= 2; td = Td(op, &pc); td.val = status_zero(~td.val); mem_w_Td(op, td); goto decode_op;
	INC:  case DECODE(0x0580): cyc -= 2; td = Td(op, &pc); td.val = add(td.val,1); mem_w_Td(op, td); goto decode_op;
	INCT: case DECODE(0x05C0): cyc -= 2; td = Td(op, &pc); td.val = add(td.val,2); mem_w_Td(op, td); goto decode_op;
	DEC:  case DECODE(0x0600): cyc -= 2; td = Td(op, &pc); td.val = sub(td.val,1); mem_w_Td(op, td); goto decode_op;
	DECT: case DECODE(0x0640): cyc -= 2; td = Td(op, &pc); td.val = sub(td.val,2); mem_w_Td(op, td); goto decode_op;
	BL:   case DECODE(0x0680): cyc -= 2; td = Td(op, &pc); Td_post_increment(op, td, 2); reg_w(11, pc); pc = td.addr; goto decode_op;
	SWPB: case DECODE(0x06C0): cyc -= 2; td = Td(op, &pc); td.val = swpb(td.val); mem_w_Td(op, td); goto decode_op;
	SETO: case DECODE(0x0700): cyc -= 2; td = Td(op, &pc); td.val = 0xffff; mem_w_Td(op, td); goto decode_op;
	ABS:  case DECODE(0x0740):
		td = Td(op, &pc);
		status_zero(td.val); // status compared to zero before operation
		clr_OV();
		clr_C(); // carry is not listed affected in manual, but it is
		//printf("%04x\n", td.val);
		if (td.val & 0x8000) { // negative? (sign bit set)
			if (td.val == 0x8000)
				set_OV();
			else
				td.val = -td.val;
			cyc += 2;
		}
		mem_w_Td(op, td);
		goto decode_op;
	SRA: case DECODE(0x0800): case DECODE(0x0880): {
		u16 val = reg_r(op & 15);
		u8 count = shift_count(op);
		if (val & (1 << (count-1))) set_C(); else clr_C();
		reg_w(op & 15, status_zero(((s16)val) >> count));
		goto decode_op; }
	SRL: case DECODE(0x0900): case DECODE(0x0980): {
		u16 val = reg_r(op & 15);
		u8 count = shift_count(op);
		if (val & (1 << (count-1))) set_C(); else clr_C();
		reg_w(op & 15, status_zero(val >> count));
		goto decode_op; }
	SLA: case DECODE(0x0A00): case DECODE(0x0A80): {
		u16 val = reg_r(op & 15);
		u8 count = shift_count(op);
		if (val & (0x8000 >> (count-1))) set_C(); else clr_C();
		reg_w(op & 15, status_zero(val << count));
		// overflow if MSB changes during shift
		if (count == 16) {
			if (val) set_OV(); else clr_OV();
		} else {
			u16 ov_mask = 0xffff << (15 - count);
			val &= ov_mask;
			if (val != 0 && val != ov_mask) set_OV(); else clr_OV();
		}
		goto decode_op; }
	SRC: case DECODE(0x0B00): case DECODE(0x0B80): {
		u16 val = reg_r(op & 15);
		u8 count = shift_count(op);
		if (val & (1 << (count-1))) set_C(); else clr_C();
		reg_w(op & 15, status_zero((val << (16-count))|(val >> count)));
		goto decode_op; }

	// RET  >0C00  0000 1100 0000 0000  new RET      Return from subroutine, pop return address from stack (stack pointer is R15)
	RET: case DECODE(0x0C00): {
		u16 sp = reg_r(15) + 2;
		pc = mem_r(sp);
		reg_w(15, sp);
		goto decode_op; }
	// CALL >0C80  0000 1100 10Ts SSSS  new CALL     Call subroutine, push return address on stack (stack pointer is R15)
	CALL: case DECODE(0x0C80): {
		cyc -= 2; td = Td(op, &pc); Td_post_increment(op, td, 2);
		u16 sp = reg_r(15);
		mem_w(sp, pc);
		reg_w(15, sp - 2);
		pc = td.addr;
		goto decode_op; }

	// PUSH >0D00  0000 1101 00Ts SSSS  new PUSH     Push a 16-bit word onto the stack
	PUSH: case DECODE(0x0D00): {
		td = Td(op, &pc); Td_post_increment(op, td, 2);
		u16 sp = reg_r(15);
		mem_w(sp, td.val);
		reg_w(15, sp - 2);
		goto decode_op; }
	// POP  >0F00  0000 1111 00Td DDDD  new POP      Pop a 16-bit word off of the stack
	POP: case DECODE(0x0F00): {
		td = Td(op, &pc);
		u16 sp = reg_r(15) + 2;
		td.val = mem_r(sp);
		reg_w(15, sp);
		mem_w_Td(op, td);
		goto decode_op; }
	// SLC  >0E00  0000 1110 00Ts SSSS  new SLC      Shift Left Circular
	SLC: case DECODE(0x0E00): case DECODE(0x0E80): {
		u16 val = reg_r(op & 15);
		u8 count = shift_count(op);
		if (val & (0x8000 >> (count-1))) set_C(); else clr_C();
		reg_w(op & 15, status_zero((val >> (16-count))|(val << count)));
		// overflow if MSB changes during shift
		if (count == 16) {
			if (val) set_OV(); else clr_OV();
		} else {
			u16 ov_mask = 0xffff << (15 - count);
			val &= ov_mask;
			if (val != 0 && val != ov_mask) set_OV(); else clr_OV();
		}
		goto decode_op; }



        JMP: case DECODE(0x1000): cyc += 2; pc += 2 * (s8)(op & 0xff); goto decode_op;
        JLT: case DECODE(0x1100): if (tst_LT()) goto JMP; goto decode_op;
        JLE: case DECODE(0x1200): if (tst_LE()) goto JMP; goto decode_op;
        JEQ: case DECODE(0x1300): if (tst_EQ()) goto JMP; goto decode_op;
        JHE: case DECODE(0x1400): if (tst_HE()) goto JMP; goto decode_op;
        JGT: case DECODE(0x1500): if (tst_GT()) goto JMP; goto decode_op;
        JNE: case DECODE(0x1600): if (!tst_EQ()) goto JMP; goto decode_op;
        JNC: case DECODE(0x1700): if (!tst_C()) goto JMP; goto decode_op;
        JOC: case DECODE(0x1800): if (tst_C()) goto JMP; goto decode_op;
        JNO: case DECODE(0x1900): if (!tst_OV()) goto JMP; goto decode_op;
        JL:  case DECODE(0x1A00): if (tst_L()) goto JMP; goto decode_op;
        JH:  case DECODE(0x1B00): if (tst_H()) goto JMP; goto decode_op;
        JOP: case DECODE(0x1C00): if (tst_OP()) goto JMP; goto decode_op;
        //SBO: case DECODE(0x1D00): cru_w((op & 0xff) + ((reg_r(12) & 0x1ffe) >> 1), 1); goto decode_op;
        //SBZ: case DECODE(0x1E00): cru_w((op & 0xff) + ((reg_r(12) & 0x1ffe) >> 1), 0); goto decode_op;
        //TB:  case DECODE(0x1F00): status_equal(cru_r((op & 0xff) + ((reg_r(12) & 0x1ffe) >> 1)), 1); goto decode_op;

	COC: case DECODE(0x2000): case DECODE(0x2200): {
		u16 ts = Ts(op, &pc);
		u16 td = reg_r((op >> 6) & 15);
		if ((ts & td) == ts) set_EQ(); else clr_EQ();
		cyc += 2;
		goto decode_op; }
	CZC: case DECODE(0x2400): case DECODE(0x2600): {
		u16 ts = Ts(op, &pc);
		u16 td = reg_r((op >> 6) & 15);
		if (!(ts & td)) set_EQ(); else clr_EQ();
		cyc += 2;
		goto decode_op; }
	XOR: case DECODE(0x2800): case DECODE(0x2A00): {
		u8 reg = (op >> 6) & 15;
		u16 ts = Ts(op, &pc);
		u16 td = reg_r(reg);
		reg_w(reg, status_zero(ts ^ td));
		goto decode_op; }
	// XOP  >2C00                       mod PIX      New dedicated pixel plotting and addressing instruction
	XOP: case DECODE(0x2C00): case DECODE(0x2E00): {
		debug_log("PIX not implemented\n");
		goto decode_op; }

	// LDCR >3000                       mod SPI_OUT  Writes a byte (always a byte operation) to the SPI Flash ROM
	LDCR: case DECODE(0x3000): case DECODE(0x3200): {
		debug_log("LDCR/SPI_OUT not implemented\n");
		goto decode_op; }
	// STCR >3400                       mod SPI_IN   Reads a byte (always a byte operation) from the SPI Flash ROM
	STCR: case DECODE(0x3400): case DECODE(0x3600): {
		debug_log("STCR/SPI_IN not implemented\n");
		goto decode_op; }
	MPY: case DECODE(0x3800): case DECODE(0x3A00): {
		u32 val = Ts(op, &pc);
		u8 reg = (op >> 6) & 15;
		//debug_log("MPY %04X x %04X = %08X\n", 
		//	val, reg_r(reg), val * reg_r(reg));
		val *= reg_r(reg);
		reg_w(reg, val >> 16);
		reg_w(reg+1, val & 0xffff);
		goto decode_op; }
	DIV: case DECODE(0x3C00): case DECODE(0x3E00): {
		u32 val;
		u16 ts = Ts(op, &pc);
		u8 reg = (op >> 6) & 15;
		val = reg_r(reg);
		if (ts <= val) {
			set_OV();
		} else {
			clr_OV();
			val = (val << 16) | reg_r(reg+1);
			//debug_log("DIV %08X by %04X\n", val, ts);
			reg_w(reg, val / ts);
			reg_w(reg+1, val % ts);
		}
		goto decode_op; }

	SZC: case DECODE(0x4000): case DECODE(0x4400): case DECODE(0x4800): case DECODE(0x4C00):
		word_op(&op, &pc, &ts, &td);
		td.val = status_zero(td.val & ~ts); mem_w_Td(op, td); goto decode_op;
	SZCB: case DECODE(0x5000): case DECODE(0x5400): case DECODE(0x5800): case DECODE(0x5C00):
		byte_op(&op, &pc, &ts, &td);
		if (td.addr & 1) {
			td.val &= ~(ts >> 8);
			status_parity(status_zero(td.val << 8));
		} else {
			td.val &= ~ts;
			status_parity(status_zero(td.val & 0xff00));
		}
		mem_w_TdB(op, td);
		goto decode_op;
	S: case DECODE(0x6000): case DECODE(0x6400): case DECODE(0x6800): case DECODE(0x6C00):
		word_op(&op, &pc, &ts, &td);
		td.val = sub(td.val, ts); mem_w_Td(op, td); goto decode_op;
	SB: case DECODE(0x7000): case DECODE(0x7400): case DECODE(0x7800): case DECODE(0x7C00):
		byte_op(&op, &pc, &ts, &td);
		if (td.addr & 1) {
			td.val = (td.val & 0xff00) | status_parity(sub(td.val << 8, ts) >> 8);
		} else {
			td.val = (td.val & 0x00ff) | status_parity(sub(td.val & 0xff00, ts));
		}
		mem_w_TdB(op, td);
		goto decode_op;
	C: case DECODE(0x8000): case DECODE(0x8800):
		word_op(&op, &pc, &ts, &td);
		status_arith(ts, td.val); cyc+=2; Td_post_increment(op, td, 2); goto decode_op;
	CB: case DECODE(0x9000): case DECODE(0x9800):
		byte_op(&op, &pc, &ts, &td);
		if (td.addr & 1) {
			status_arith(status_parity(ts), td.val << 8);
		} else {
			status_arith(status_parity(ts), td.val & 0xff00);
		}
		cyc += 2;
		Td_post_increment(op, td, 1);
		goto decode_op;
	A: case DECODE(0xA000): case DECODE(0xA800):
		word_op(&op, &pc, &ts, &td);
		td.val = add(td.val, ts); mem_w_Td(op, td); goto decode_op;
	AB: case DECODE(0xB000): case DECODE(0xB800):
		byte_op(&op, &pc, &ts, &td);
		if (td.addr & 1) {
			td.val = (td.val & 0xff00) | status_parity(add(td.val << 8, ts) >> 8);
		} else {
			td.val = (td.val & 0x00ff) | status_parity(add(td.val & 0xff00, ts));
		}
		mem_w_TdB(op, td);
		goto decode_op;
	MOV: case DECODE(0xC000): case DECODE(0xC800):
		word_op(&op, &pc, &ts, &td);
		td.val = status_zero(ts); mem_w_Td(op, td); goto decode_op;
	MOVB: case DECODE(0xD000): case DECODE(0xD800):
		byte_op(&op, &pc, &ts, &td);
		if (td.addr & 1) {
			td.val = (td.val & 0xff00) | status_parity(status_zero(ts) >> 8);
		} else {
			td.val = (td.val & 0x00ff) | status_parity(status_zero(ts));
		}
		mem_w_TdB(op, td);
		goto decode_op;
	SOC: case DECODE(0xE000): case DECODE(0xE800):
		word_op(&op, &pc, &ts, &td);
		td.val = status_zero(td.val | ts); mem_w_Td(op, td); goto decode_op;
	SOCB: case DECODE(0xF000): case DECODE(0xF800):
		byte_op(&op, &pc, &ts, &td);
		if (td.addr & 1) {
			td.val |= (ts >> 8);
			status_parity(status_zero(td.val << 8));
		} else {
			td.val |= ts;
			status_parity(status_zero(td.val & 0xff00));
		}
		mem_w_TdB(op, td);
		goto decode_op;

	default:
		//printf("GPU %04x: %04x  %d\n", pc-2, op, (int)__builtin_clz(op));
		UNHANDLED:
		goto decode_op;
	}
done:
	gPC = pc;
	return;
}



static const char **names[] = {
	(const char *[]){"C", "CB", "A", "AB", "MOV", "MOVB", "SOC", "SOCB"},
	(const char *[]){"SZC", "SZCB", "S", "SB"},
	(const char *[]){"COC", "CZC", "XOR", "XOP", "LDCR", "STCR", "MPY", "DIV"},
	(const char *[]){"JMP", "JLT", "JLE", "JEQ", "JHE", "JGT", "JNE", "JNC",
			 "JOC", "JNO", "JL", "JH", "JOP", "SBO", "SBZ", "TB"},
	(const char *[]){"SRA", "SRL", "SLA", "SRC"},
	(const char *[]){"BLWP", "B", "X", "CLR", "NEG", "INV", "INC", "INCT",
			 "DEC", "DECT", "BL", "SWPB", "SETO", "ABS", NULL, NULL},
	(const char *[]){"LI", "AI", "ANDI", "ORI", "CI", "STWP", "STST", "LWPI",
			"LIMI", NULL, "IDLE", "RSET", "RTWP", "CKON", "CKOF", "LREX"},
};
static const struct {
	u8 shift, mask, cycles;
} decode[] = {        //          0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |10 |11 |12 |13 |14 |15 |
	{12, 0x7, 6}, // 1 arith  1 |opcode | B |  Td   |       D       |  Ts   |       S       |
	{12, 0x3, 6}, // 2 arith  0   1 |opc| B |  Td   |       D       |  Ts   |       S       |
	{10, 0x7, 6}, // 3 math   0   0   1 | --opcode- |     D or C    |  Ts   |       S       |
	{ 8, 0xf, 6}, // 4 jump   0   0   0   1 | ----opcode--- |     signed displacement       |
	{ 8, 0x7, 6}, // 5 shift  0   0   0   0   1 | --opcode- |       C       |       W       |
	{ 6, 0xf, 6}, // 6 pgm    0   0   0   0   0   1 | ----opcode--- |  Ts   |       S       |
	{ 5, 0xf, 4}, // 7 ctrl   0   0   0   0   0   0   1 | ----opcode--- |     not used      |
};		      // 7 ctrl   0   0   0   0   0   0   1 | opcode & immd | X |       W       |

#define L4(a,b,c,d) &&a-&&C,&&b-&&C,&&c-&&C,&&d-&&C
#define L8(a,b,c,d,e,f,g,h) L4(a,b,c,d),L4(e,f,g,h)


static u16 save_r(u16 address)
{
	return mem_r(address);
}

static u16 disasm_Ts(u16 pc, u16 op, int opsize)
{
	u16 i = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		sprintf(gpu_text+strlen(gpu_text), "R%d", i);
		break;
	case 1: // *Rx
		sprintf(gpu_text+strlen(gpu_text), "*R%d", i);
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		pc += 2;
		sprintf(gpu_text+strlen(gpu_text), "@>%04X", safe_r(pc));
		if (i) {
			sprintf(gpu_text+strlen(gpu_text), "(R%d)", i);
		}
		break;
	case 3: // *Rx+
		sprintf(gpu_text+strlen(gpu_text), "*R%d+", i);
		break;
	}
	return pc;
}

static u16 disasm_Bs(u16 pc, u16 op, int opsize)
{
	u16 i = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		sprintf(gpu_text+strlen(gpu_text), "R%d", i);
		break;
	case 1: // *Rx
		sprintf(gpu_text+strlen(gpu_text), "*R%d", i);
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		pc += 2;
		sprintf(gpu_text+strlen(gpu_text), "@>%04X", safe_r(pc));
		if (i) {
			sprintf(gpu_text+strlen(gpu_text), "(R%d)", i);
		}
		break;
	case 3: // *Rx+
		sprintf(gpu_text+strlen(gpu_text), "*R%d+", i);
		break;
	}
	return pc;
}


// returns number of bytes in the disassembled instruction (2, 4, or 6)
static int gpu_disasm(u16 pc)
{
	static const int jt1[]={L8(C,CB,A,AB,MOV,MOVB,SOC,SOCB)};
	static const int jt2[]={L4(SZC,SZCB,S,SB)};
	static const int jt3[]={L8(COC,CZC,XOR,XOP,LDCR,STCR,MPY,DIV)};
	static const int jt4[]={L8(JMP,JLT,JLE,JEQ,JHE,JGT,JNE,JNC),L8(JOC,JNO,JL,JH,JOP,SBO,SBZ,TB)};
	static const int jt5[]={L4(SRA,SRL,SLA,SRC)};
	static const int jt6[]={L8(BLWP,B,X,CLR,NEG,INV,INC,INCT),L8(DEC,DECT,BL,SWPB,SETO,ABS,BAD,BAD)};
	static const int jt7[]={L8(LI,AI,ANDI,ORI,CI,STWP,STST,LWPI),L8(LIMI,BAD,IDLE,RSET,RTWP,CKON,CKOF,LREX)};
	static const int *jt[] = {jt1, jt2, jt3, jt4, jt5, jt6, jt7};
	u16 op = safe_r(pc), ts, td;
	u8 idx = __builtin_clz(op)-16;
	u16 start_pc = pc;

	gpu_text[0] = 0; // clear the disassembly string
	// note: for breakpoints to work, PC must be at column 5 or 6
	if (pc >= 0x6000 && pc < 0x8000)
		sprintf(gpu_text+strlen(gpu_text), " %-3d %04X  %04X  ",
		 	get_cart_bank(), pc, op);
	else
		sprintf(gpu_text+strlen(gpu_text), "     %04X  %04X  ", pc, op);
	if (idx >= ARRAY_SIZE(jt)) {
		BAD:
		sprintf(gpu_text+strlen(gpu_text), "DATA >%04X", op);
		goto done;
	}
	u8 subidx = (op >> decode[idx].shift) & decode[idx].mask;

	if (!names[idx][subidx])
		goto BAD;
	sprintf(gpu_text+strlen(gpu_text), "%-5s", names[idx][subidx]);
	goto *(&&C + jt[idx][subidx]);

C: A: MOV: SOC: SZC: S:
	pc = disasm_Ts(pc, op, 2);
	sprintf(gpu_text+strlen(gpu_text), ",");
	pc = disasm_Ts(pc, op >> 6, 2);
	goto done;

CB: AB: MOVB: SOCB: SZCB: SB:
	pc = disasm_Ts(pc, op, 1);
	sprintf(gpu_text+strlen(gpu_text), ",");
	pc = disasm_Ts(pc, op >> 6, 1);
	goto done;
COC: CZC: XOR: MPY: DIV:
	pc = disasm_Ts(pc, op, 2);
	sprintf(gpu_text+strlen(gpu_text), ",R%d", (op >> 6) & 15);
	goto done;
XOP:
	pc = disasm_Ts(pc, op, 2);
	sprintf(gpu_text+strlen(gpu_text), ",%d", (op >> 6) & 15);
	goto done;
LDCR: STCR:
	pc = disasm_Ts(pc, op, 2);
	sprintf(gpu_text+strlen(gpu_text), ",%d", (op >> 6) & 15 ?: 16);
	goto done;

JMP: JLT: JLE: JEQ: JHE: JGT: JNE: JNC: JOC: JNO: JL: JH: JOP:
	sprintf(gpu_text+strlen(gpu_text), ">%04X", pc + 2 + 2 * (s8)(op & 0xff));
	goto done;
SBO: SBZ: TB:
	sprintf(gpu_text+strlen(gpu_text), "%d", op & 0xff);
	goto done;
SRA: SRL: SLA: SRC:
	sprintf(gpu_text+strlen(gpu_text), "R%d,", op & 15);
	sprintf(gpu_text+strlen(gpu_text), (op & 0x00f0) ? "%d" : "R0", (op >> 4) & 15);
	goto done;
BLWP: B: BL:
	pc = disasm_Bs(pc, op, 2);
	goto done;
X: CLR: NEG: INV: INC: INCT: DEC: DECT: SWPB: SETO: ABS:
	pc = disasm_Ts(pc, op, 2);
	goto done;
LI: AI: ANDI: ORI: CI:
	pc += 2;
	sprintf(gpu_text+strlen(gpu_text), "R%d,>%04X", op & 15, safe_r(pc));
	goto done;
STWP: STST:
	sprintf(gpu_text+strlen(gpu_text), "R%d", op & 15);
	goto done;
IDLE: RSET: RTWP: CKON: CKOF: LREX:
	goto done;
LWPI: LIMI:
	pc += 2;
	sprintf(gpu_text+strlen(gpu_text), ">%04X", safe_r(pc));
	goto done;

done:
	sprintf(gpu_text+strlen(gpu_text), "\n");
	//sprintf(asm_text+strlen(asm_text), "\t\t%s\n", reg_text);
	//sprintf(asm_text+strlen(asm_text), "\t\t(%d)\n", disasm_cyc);
	int ret = pc+2 - start_pc;
	while (start_pc != pc) {
		start_pc += 2;
		sprintf(gpu_text+strlen(gpu_text), "           %04X\n", safe_r(start_pc));
	}
	
	return ret;
}

#endif
