/*
 *  bulwip.c - TI 99/4A emulator
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

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif
#define _GNU_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>


#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "cpu.h"


#ifdef ENABLE_GIF
#include "gif/gif.h"
static GifWriter gif;
#endif




#ifdef COMPILED_ROMS
#include "994arom.h"
#include "994agrom.h"
#include "mbtest.h"
//#include "megademo.h"

static const struct {
	const char *filename;
	const void *data;
	unsigned int len;
} comp_roms[] = {
	{"994arom.bin", rom994a, sizeof(rom994a)},
	{"994agrom.bin", grom994a, sizeof(grom994a)},
	{"mbtest.bin", mbtest, sizeof(mbtest)},
//	{"megademo8.bin", megademo, sizeof(megademo)},
};

#endif

static FILE *log = NULL;
static FILE *disasmf = NULL;

int debug_log(const char *fmt, ...)
{
	va_list ap;
	if (!log) return 0;
	va_start(ap, fmt);
	int ret = vfprintf(log, fmt, ap);
	va_end(ap);
	return ret;
}

static void* my_realloc(void *p, size_t size) { return realloc(p, size); }
static void my_free(void *p) { free(p); }
static char* my_strdup(const char *p)
{
#ifdef _WIN32
	return _strdup(p);
#else
	return strdup(p);
#endif
}


// 3000000 cycles per second
// 59.94 frames per second (59.922743404)
// pixel clock (5369317.5 Hz)
// frame time 16.6681545 ms
// line time 63.695246 us
// 262 lines per frame (342 pixels/line)
// 191.085744275 cpu cycles per line
// 0.558730246 cpu cycles per pixel
// 1.789772448 pixels / cpu cycle
#define CYCLES_PER_LINE 191


static u16 tms9901_int_mask = 0; // 9901 interrupt mask

#ifdef ENABLE_DEBUGGER
int debug_en = 0;
int debug_break = DEBUG_RUN;
#endif

static u16 fast_ram[128] = {}; // 256 bytes at 8000-80ff,repeated at 8100,8200,8300

static u16 *ram = NULL; // 32k RAM or SAMS (TODO)
static unsigned int ram_size = 0; // in bytes

char *cartridge_name = NULL;
static u16 *cart_rom = NULL;
static unsigned int cart_rom_size = 0;
static u8 *cart_grom = NULL;
static unsigned int cart_grom_size = 0;
static u16 cart_bank_mask = 0;
static u16 cart_bank = 0; // up to 512MB cart size

static u16 *rom = NULL;
static unsigned int rom_size = 0;

static u8 *grom = NULL;
static unsigned int grom_size = 0;
static u8 grom_latch = 0, grom_last = 0x00;
static u16 ga; // grom address

// keyboard and CRU
u8 keyboard[8] = {0};
static u8 keyboard_row = 0;
static u8 timer_mode = 0;
static u8 alpha_lock = 0;
static unsigned int total_cycles = 0;
static volatile unsigned int total_cycles_busy = 0;

#define VDP_RAM_SIZE (16*1024)
#define VDP_ST 8
static struct vdp {
	u8 ram[VDP_RAM_SIZE];
	u16 a; // address
	u8 latch;
	u8 reg[9]; // vdp status is [8]
	u8 y; // scanline counter
} vdp = {
	.ram = {},
	.a = 0,
	.latch = 0,
	.reg = {0,0,0,0,1,0,0,0,0},
	.y = 0,
};

struct state {
	u16 pc, wp, st;
	u16 fast_ram[128];
	u16 ram[32*1024/2];
	u16 cart_bank;
	u8 grom_latch, grom_last;
	u16 ga;
	u8 keyboard_row;
	u8 timer_mode;
	u8 alpha_lock;
	struct vdp vdp;
	int cyc;
};

struct state* save_state(struct state *s)
{
	if (!s) s = malloc(sizeof(struct state));
	memset(s, 0, sizeof(struct state));
	s->pc = get_pc();
	s->wp = get_wp();
	s->st = get_st();
	memcpy(s->fast_ram, fast_ram, sizeof(fast_ram));
	memcpy(s->ram, ram, sizeof(s->ram));
	s->cart_bank = cart_bank;
	s->grom_latch = grom_latch;
	s->grom_last = grom_last;
	s->ga = ga;
	s->keyboard_row = keyboard_row;
	s->timer_mode = timer_mode;
	s->alpha_lock = alpha_lock;
	memcpy(&s->vdp, &vdp, sizeof(struct vdp));
	s->cyc = add_cyc(0);
	return s;
}

void load_state(struct state *s)
{
	// cpu.c (undo access only!)
	extern void set_pc(u16);
	extern void set_wp(u16);
	extern void set_st(u16);
	extern void set_cyc(s16);

	set_pc(s->pc);
	set_wp(s->wp);
	set_st(s->st);

	memcpy(fast_ram, s->fast_ram, sizeof(fast_ram));
	memcpy(ram, s->ram, sizeof(s->ram));
	cart_bank = s->cart_bank;
	grom_latch = s->grom_latch;
	grom_last = s->grom_last;
	ga = s->ga;
	keyboard_row = s->keyboard_row;
	timer_mode = s->timer_mode;
	alpha_lock = s->alpha_lock;
	memcpy(&vdp, &s->vdp, sizeof(struct vdp));
	set_cyc(s->cyc);
}

void print_state(FILE *f, struct state *s)
{
	int i;
	fprintf(f, "PC = %04X\nWP = %04X\nST = %04X\ncyc = %d\n", s->pc, s->wp, s->st, s->cyc);
	for (i = 0; i < 256/16; i++) {
		u16 *p = s->fast_ram+i*8;
		fprintf(f, "%04X: %04x %04x %04x %04x  %04x %04x %04x %04x\n",
			0x8300+i*16,p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
	}
	fprintf(f,
		"cart_bank = %04X\n"
		"grom_latch = %d\n"
		"grom_last = %02X\n"
		"grom_addr = %04X\n"
		"keyboard_row = %d\n"
		"timer_mode = %d\n"
		"alpha_lock = %d\n"
		"VDP_ADDR = %04X\n"
		"VDP_latch = %d\n"
		"VDP_Y = %d\n"
		"VDP_REG[0] = %02X\n"
		"VDP_REG[1] = %02X\n"
		"VDP_REG[2] = %02X\n"
		"VDP_REG[3] = %02X\n"
		"VDP_REG[4] = %02X\n"
		"VDP_REG[5] = %02X\n"
		"VDP_REG[6] = %02X\n"
		"VDP_REG[7] = %02X\n"
		"VDP_ST = %02X\n",
		s->cart_bank,
		s->grom_latch,
		s->grom_last,
		s->ga,
		s->keyboard_row,
		s->timer_mode,
		s->alpha_lock,
		s->vdp.a,
		s->vdp.latch,
		s->vdp.y,
		s->vdp.reg[0],
		s->vdp.reg[1],
		s->vdp.reg[2],
		s->vdp.reg[3],
		s->vdp.reg[4],
		s->vdp.reg[5],
		s->vdp.reg[6],
		s->vdp.reg[7],
		s->vdp.reg[VDP_ST]);
	for (i = 0; i < 16*1024/16; i++) {
		u8 *p = s->vdp.ram+i*16;
		fprintf(f, "V%04X: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
			i*16,p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
	}
	for (i = 0; i < 32*1024; i+=16) {
		u16 *p = s->ram+(i/2);
		fprintf(f, "%04X: %04x %04x %04x %04x  %04x %04x %04x %04x\n",
			(i<8192 ? 0x2000 : 0xa000-8192)+i,p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7]);
	}
}


/******************************************
 * Undo stack and encoding functions      *
 ******************************************/


#ifdef ENABLE_UNDO

static unsigned int undo_buffer[1024*1024] = {};
static unsigned int undo_head = 0, undo_tail = 0;

int undo_pop(void)
{
	// cpu.c (undo access only!)
	extern void set_pc(u16);
	extern void set_wp(u16);
	extern void set_st(u16);
	extern void set_cyc(s16);

	while (undo_head != undo_tail) {
		if (undo_head-- == 0) { // wrap around
			undo_head = ARRAY_SIZE(undo_buffer)-1;
		}
		unsigned int u = undo_buffer[undo_head];
		u16 v = u >> 16;
		u16 w = u & 0xffff;
		//printf("undo %04x %04x\n", v>>16, w);
		switch (v) {
		case UNDO_PC: set_pc(w); return 0;
		case UNDO_WP: set_wp(w); break;
		case UNDO_ST: set_st(w); break;
		case UNDO_CYC: set_cyc(w); break;
		case UNDO_VDPA: vdp.a = w; break;
		case UNDO_VDPD: /* ??? */ break;
		case UNDO_VDPL: vdp.latch = w & 1; break;
		case UNDO_VDPST: vdp.reg[VDP_ST] = w; break;
		case UNDO_VDPY: vdp.y = w; break;
		case UNDO_VDPR: vdp.reg[(w>>8)] = w&0xff; break;
		case UNDO_GA: ga = w; break;
		case UNDO_GD: grom_last = w; break;
		case UNDO_GL: grom_latch = w&1; break;
		case UNDO_CB: cart_bank = w; break;
		case UNDO_KB: keyboard_row = w; break;
		default:
			if ((v & 0xc000) == UNDO_EXPRAM) {
				//printf("undo exp a=%x w=%04X\n", v&0x3fff, w);
				ram[v & 0x3fff] = w;
			} else if ((v & 0xf000) == UNDO_VDPRAM) {
				vdp.ram[(u>>8) & 0x3fff] = u & 0xff;
			} else if ((v & 0xff80) == UNDO_CPURAM) {
				//printf("fast_ram[%02x]=%04x\n", v & 0x7f, w);
				fast_ram[v & 0x7f] = w;
			} else {
				printf("unhandled undo %04x %04x\n", v, w);
			}
			break;
		}
	}
	printf("undo buffer exhausted - \n");
	return -1;
}

// returns a list of PCs and cycle counts from the undo stack
// this is used to show the debugger instruction trace window with cycle counts
void undo_pcs(u16 *pcs, u8 *cycs, int count)
{
	int idx = 0;
	unsigned int i = undo_head;
	int cyc = add_cyc(0);
	while (i != undo_tail) {
		if (i-- == 0) { // wrap around
			i = ARRAY_SIZE(undo_buffer)-1;
		}
		unsigned int v = undo_buffer[i];
		unsigned int w = undo_buffer[i] & 0xffff;
		if ((v >> 16) == UNDO_PC) {
			pcs[idx++] = w;
			if (idx == count) break;
		} else if ((v >> 16) == UNDO_CYC) {
			cycs[idx] = cyc - (s16)w;
			cyc = (s16)w;
		} else if ((v >> 16) == UNDO_VDPY) {
			cyc += CYCLES_PER_LINE;
		}
	}
}

void undo_push(u16 op, unsigned int value)
{
	undo_buffer[undo_head] = (op << 16) | value;
	if (++undo_head == ARRAY_SIZE(undo_buffer)) {
		undo_head = 0; // wrap around
	}
	if (undo_tail == undo_head) {
		if (++undo_tail == ARRAY_SIZE(undo_buffer)) {
			undo_tail = 0; // wrap around
		}
	}
}

// This is used to update the last cycle count pushed,
// after the instruction has finished executing
void undo_fix_cyc(u16 cyc)
{
	unsigned int i = undo_head;
	while (i != undo_tail) {
		if (i-- == 0) { // wrap around
			i = ARRAY_SIZE(undo_buffer)-1;
		}
		unsigned int v = undo_buffer[i];
		if ((v >> 16) == UNDO_CYC) {
			undo_buffer[i] &= 0xffff0000;
			undo_buffer[i] |= cyc;
			break;
		}
	}
}

#endif

unsigned int get_total_cpu_cycles(void)
{
	// The total cycles and current cycle count are updated independently
	// at the end of each scan line.  So this could lead to a race condition
	// where they are temporarily out of sync while both are updated.
	// The 'busy' variable is set to the correct value during this update sequence and cleared after.
	if (total_cycles_busy)
		return total_cycles_busy;
	// otherwise combine 'total_cycles' and current cycle counter
	return total_cycles + add_cyc(0);
}


/******************************************
 * 8000-83FF  fast RAM                    *
 ******************************************/

static u16 ram_8300_r(u16 address)
{
	// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
	add_cyc(2); // 2 cycles for memory access
	//debug_log("RAM read %04X = %04X\n", address & 0xfffe, fast_ram[(address & 0xfe) >> 1]);
	return fast_ram[(address & 0xfe) >> 1];
}

static void ram_8300_w(u16 address, u16 value)
{
	//if (trace) printf("%04x <= %04x\n", address, value);
	// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
	add_cyc(2); // 2 cycles for memory access
	address = (address & 0xfe) >> 1;
	undo_push(UNDO_CPURAM + address, fast_ram[address]);
	fast_ram[address] = value;
//		if (address == 0x8380) {
//			debug_log("RAM write %04X = %04X\n", address & 0xfffe, value);
//			dump_regs();
//		}
}



static u16 sound_8400_r(u16 address)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	if (address == 0x8400) {
		// sound chip (illegal to read?)
		return 0;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static void sound_8400_w(u16 address, u16 value)
{
	if (address == 0x8400) {
		// sound chip
		add_cyc(34);
#ifdef USE_SDL
		snd_w(value >> 8);
#endif
	} else {
		add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	}
}



static u16 vdp_8800_r(u16 address)
{
	add_cyc(6); // 2 cycles for memory access + 4 for ?
	vdp.latch = 0;
	if (address == 0x8800) {
		// 8800   VDP RAM read data register
		undo_push(UNDO_VDPA, vdp.a);
		return vdp.ram[vdp.a++ & 0x3fff] << 8;
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
		u16 value = vdp.reg[VDP_ST] << 8;
		//debug_log("VDP_STATUS=%0x\n", vdp.reg[VDP_ST]);
		undo_push(UNDO_VDPST, vdp.reg[VDP_ST]);
		vdp.reg[VDP_ST] &= ~0xE0; // clear interrupt flags
		interrupt(-1); // deassert INTREQ
		return value;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static u16 vdp_8800_safe_r(u16 address)
{
	//add_cyc(6);
	vdp.latch = 0;
	if (address == 0x8800) {
		// 8800   VDP RAM read data register
		return vdp.ram[vdp.a & 0x3fff] << 8;
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
		return vdp.reg[VDP_ST] << 8;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}


static void vdp_8800_w(u16 address, u16 value)
{
	add_cyc(6); // 2 cycles for memory access + 4 for ?
	if (address == 0x8800) {
		// 8800   VDP RAM read data register
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
	}
}


static u16 vdp_8c00_r(u16 address)
{
	add_cyc(6); // 2 cycles for memory access + 4 for ?
	if (address == 0x8C00) {
		// 8C00   VDP RAM write data register
		return 0;
	} else if (address == 0x8C02) {
		// 8C02   VDP RAM write address register
		return 0;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static void vdp_8c00_w(u16 address, u16 value)
{
	add_cyc(6); // 2 cycles for memory access + 4 for ?
	if (address == 0x8C00) {
		// 8C00   VDP RAM write data register
		//debug_log("VDP write %04X = %02X\n", vdp.a, value >> 8);
		undo_push(UNDO_VDPA, vdp.a);
		undo_push(UNDO_VDPL, vdp.latch);
		undo_push(UNDO_VDPRAM, (vdp.a << 8) | vdp.ram[vdp.a]);
		vdp.ram[vdp.a] = value >> 8;
		vdp.a = (vdp.a + 1) & 0x3fff; // wraps at 16K
		vdp.latch = 0;
		return;
	} else if (address == 0x8C02) {
		// 8C02   VDP RAM write address register
		undo_push(UNDO_VDPA, vdp.a);
		undo_push(UNDO_VDPL, vdp.latch);
		vdp.latch ^= 1;
		if (vdp.latch) {
			// first low byte 
			vdp.a = value >> 8;
		} else {
			// second high byte
			if (value & 0x8000) {
				// register write
				undo_push(UNDO_VDPR, (value & 0x700) | vdp.reg[(value >> 8) & 7]);
				vdp.reg[(value >> 8) & 7] = vdp.a & 0xff;
#ifdef TRACE_VDP
				fprintf(stdout, "VDP register %X %02X at PC=%04X\n", (value >> 8) & 7, vdp.a & 0xff, get_pc());
#endif
			} else if (value & 0x4000) {
				// set address for data write
				vdp.a |= (value & 0x3f00);
				//debug_log("VDP address for write %04X\n", vdp.a);
			} else {
				// set address for data read
				vdp.a |= (value & 0x3f00);
				// FIXME: this should start reading the data and increment the address
				//debug_log("VDP address for read %04X at PC=%04X\n", vdp.a, get_pc());
			}
		}
		return;
	}
}


static u16 speech_9000_r(u16 address)
{
	if (address == 0x9000) {
		// speech
		add_cyc(54);
		// TODO
		return 0;
	}
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	return 0;
}

static void speech_9000_w(u16 address, u16 value)
{
	if (address == 0x9000) {
		// speech
		add_cyc(54);
		// TODO
		return;
	}
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	return;
}

/******************************************
 * 9800-9FFF  GROM                        *
 ******************************************/

static u16 safe_grom_9800_r(u16 address)
{
	if ((address & 0xff03) == 0x9800) {
		// grom read data
		return grom_last << 8;

	} else if ((address & 0xff03) == 0x9802) {
		// grom read address (plus one) (first low byte, then high)
		return (ga & 0xff00);

	} else if ((address & 0xff03) == 0x9c00 || (address & 0xff03) == 0x9c02) {
		// grom write data / address
		return 0;
	}
	return 0;
}


static u8 grom_read(void)
{
	if (ga < grom_size) {
		grom_last = grom[ga];
	} else if (ga - grom_size < cart_grom_size) {
		//printf("cart grom %04X %02X\n", ga-grom_size, cart_grom[ga-grom_size]);
		grom_last = cart_grom[ga-grom_size];
	} else {
		grom_last = 0;
	}
	return grom_last;
}

static void grom_address_increment(void)
{
	// increment and wrap around
	ga = (ga & 0xe000) | ((ga+1) & 0x1fff);
}

static u16 grom_9800_r(u16 address)
{
	// Console GROMs will map at GROM addresses >0000-5FFF in any base
	// Cartridge GROMs are only available in GROM base 0
	if ((address & 3) == 0 && (ga < 0x6000 || address == 0x9800)) {
		// grom read data
#ifdef TRACE_GROM
		debug_log("%04X GROM read %04X %02X\n", get_pc(), ga, grom_last);
#endif
		// pc=4e8  GET NEXT BYTE FROM GAME ROM
		// pc=556  LOAD LOOP ADDRESS
		// pc=55a   2nd byte?
		// pc=77c  GET NEXT BYTE FROM GROM (2ND BYTE)

		u16 value = grom_last << 8;
		undo_push(UNDO_GD, grom_last);
		grom_read();
		undo_push(UNDO_GA, ga);
		grom_address_increment();

		add_cyc(25);
		undo_push(UNDO_GL, grom_latch);
		grom_latch = 0;
		//if ((ga&0xf000) == 0x6000) printf("GDATA %04x %04x %04x %c\n", address, ga, value, value>>8);
		return value;
	} else if ((address & 3) == 2) {
		// grom read address (plus one) (first low byte, then high)
		u16 value = (ga & 0xff00);
		add_cyc(19);
		undo_push(UNDO_GA, ga);
		undo_push(UNDO_GL, grom_latch);
		grom_latch = 0;
		ga = (ga << 8) | (ga & 0x00ff);
		//debug_log("%04X GROM addr read %04X\n", get_pc(), ga);
		//if ((ga&0xf000) == 0x6000) printf("GADDR %04x %04x %04x %c\n", address, ga, value, value>>8);
		return value;
	}
	add_cyc(6);
	return 0;
}

static u16 grom_9c00_r(u16 address)
{
	if ((address & 3) == 0 || (address & 3) == 2) {
		// grom write data / address
		add_cyc(25);
		return 0;
	}
	add_cyc(6);
	return 0;
}


static void grom_9c00_w(u16 address, u16 value)
{
	if ((address & 3) == 0) {
		// grom write data
		//debug_log("%04X GROM write %04X %02X\n", get_pc(), ga, value);
#ifdef GROM_WRITE_SUPPORTED
		if (ga >= 0x6000) {
			grom[ga] = value >> 8;
			// increment and wrap around
			ga = (ga & 0xe000) | ((ga+1) & 0x1fff);
		}
#endif
		add_cyc(28);
		return;
	} else if ((address & 3) == 2) {
		// grom write address
		undo_push(UNDO_GA, ga);
		undo_push(UNDO_GL, grom_latch);
		ga = ((ga << 8) & 0xff00) | (value >> 8);

		grom_latch ^= 1;
		if (grom_latch) {
			// first
			add_cyc(21);
		} else {
			// second
			add_cyc(27);

			undo_push(UNDO_GD, grom_last);
			grom_read();
			grom_address_increment();

			//printf("%04X GROM write address %04X %02X\n", get_pc(), ga, value>>8);

		}
#ifdef TRACE_GROM
		debug_log("%04X GROM write address %04X %02X\n", get_pc(), ga, value>>8);
#endif
		return;
	}
	add_cyc(6);
}




/****************************************
 * 0000-1FFF  ROM                       *
 ****************************************/

static u16 rom_r(u16 address)
{
	add_cyc(2); // 2 cycles for memory access
	//printf("%04X => %04X\n", rom[address >> 1]);
	return rom[address>>1];
}

static void rom_w(u16 address, u16 value)
{
	// rom not writable
	debug_log("ROM write %04X %04X at pc=%x\n", address, value, get_pc());
	//printf("ROM write %04X %04X at pc=%x\n", address, value, get_pc());
	//trace = 1;
	add_cyc(2);  // 2 cycles for memory access
}

/****************************************
 * 4000-5FFF  DSR ROM (paged by CRU?)   *
 ****************************************/

static u16 dsr_rom_r(u16 address)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	// TODO each DSR peripheral may do different things based on CRU
	return 0;
}

static void dsr_rom_w(u16 address, u16 value)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	// TODO each DSR peripheral may do different things based on CRU
}


/****************************************
 * 6000-7FFF  Cartridge ROM/RAM         *
 ****************************************/

static void set_cart_bank(u16 bank)
{
	// 8K banking
	cart_bank = bank & cart_bank_mask;
	u16 *base = cart_rom + cart_bank * 4096/*words per 8KB bank*/;
	//printf("%s: address=%04X bank=%d\n", __func__, bank, cart_bank);
	change_mapping(0x6000, 0x2000, base);
}

static void cart_rom_w(u16 address, u16 value)
{
	undo_push(UNDO_CB, cart_bank);
	set_cart_bank(address >> 1);
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	//debug_log("Cartridge ROM write %04X %04X\n", address, value);
}




/****************************************
 * 2000-3FFF,A000-FFFF  Expansion RAM   *
 ****************************************/

// extra indirection for undo
static void exp_w(u16 address, u16 value)
{
	u16 a = (address < 0xa000 ? address - 0x2000 : address - 0x8000) >> 1;
	//printf("%s: a=%x ram[a]=%04X address=%04X value=%04X\n",
	//		__func__, a, ram[a], address, value);
	undo_push(UNDO_EXPRAM + a, ram[a]);
	map_w(address, value);
}

static u16 zero_r(u16 address)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	return 0;
}

static void zero_w(u16 address, u16 value)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	return;
}


/****************************************
 * Initialize memory function table     *
 ****************************************/

static void mem_init(void)
{
	ram_size = 32 * 1024; // 32K 
	ram = malloc(ram_size);

	// system ROM 0000-1fff
	set_mapping(0x0000, 0x2000, rom_r, rom_w, NULL);

	// low memory expansion 2000-3fff
	set_mapping(0x2000, 0x2000, map_r, exp_w, ram);

	// DSR ROM 4000-5fff
	set_mapping(0x4000, 0x2000, zero_r, zero_w, NULL);

	// cartridge ROM 6000-7fff
	set_mapping(0x6000, 0x2000, map_r, cart_rom_w, NULL);

	// memory mapped devices 8000-9fff
	set_mapping(0x8000, 0x400, ram_8300_r, ram_8300_w, NULL);
	set_mapping_safe(0x8400, 0x400, sound_8400_r, zero_r, sound_8400_w, NULL);
	set_mapping_safe(0x8800, 0x400, vdp_8800_r, vdp_8800_safe_r, vdp_8800_w, NULL);
	set_mapping_safe(0x8c00, 0x400, vdp_8c00_r, zero_r, vdp_8c00_w, NULL);
	set_mapping_safe(0x9000, 0x400, speech_9000_r, zero_r, speech_9000_w, NULL);
	set_mapping(0x9400, 0x400, zero_r, zero_w, NULL);
	set_mapping_safe(0x9800, 0x400, grom_9800_r, safe_grom_9800_r, zero_w, NULL);
	set_mapping(0x9c00, 0x400, zero_r, grom_9c00_w, NULL);

	// high memory expansion A000-ffff
	set_mapping(0xa000, 0x6000, map_r, exp_w, ram+(0x2000/2));

	// instruction aquisition func
	//iaq_func = mem_r;
}


/****************************************
 * Keyboard                             *
 ****************************************/

void set_key(int key, int val)
{
	int row = (key >> 3) & 7, col = key & 7, mask = val << col;
	keyboard[row] = (keyboard[row] & ~(1 << col)) | mask;
	//alpha_lock = !!(key & TI_ALPHALOCK); // FIXME
}

void reset_ti_keys(void)
{
	memset(&keyboard, 0, sizeof(keyboard)); // clear keys
}

int ti_key_pressed(void)
{
	static const u8 zero[ARRAY_SIZE(keyboard)] = {};
	return !memcmp(zero, keyboard, ARRAY_SIZE(keyboard));
}

int char2key(char ch)
{
	char map[] = {
		TI_SPACE, // ' '
		TI_ADDSHIFT|TI_1,  // !
		TI_ADDFCTN|TI_P,  // "
		TI_ADDSHIFT|TI_3,  // #
		TI_ADDSHIFT|TI_4,  // $
		TI_ADDSHIFT|TI_5,  // %
		TI_ADDSHIFT|TI_7,  // &
		TI_ADDFCTN|TI_O,  // '
		TI_ADDSHIFT|TI_9,  // (
		TI_ADDSHIFT|TI_0,  // )
		TI_ADDSHIFT|TI_8,  // *
		TI_ADDSHIFT|TI_EQUALS,  // +
		TI_COMMA,  // ,
		TI_ADDSHIFT|TI_SLASH,  // -
		TI_PERIOD,  // .
		TI_SLASH,  // /
		TI_0,  // 0
		TI_1,  // 1
		TI_2,  // 2
		TI_3,  // 3
		TI_4,  // 4
		TI_5,  // 5
		TI_6,  // 6
		TI_7,  // 7
		TI_8,  // 8
		TI_9,  // 9
		TI_ADDSHIFT|TI_SEMICOLON,  // :
		TI_SEMICOLON,  // ;
		TI_ADDSHIFT|TI_COMMA,  // <
		TI_EQUALS,  // =
		TI_ADDSHIFT|TI_PERIOD,  // >
		TI_ADDFCTN|TI_I,  // ?
		TI_ADDSHIFT|TI_2,  // @
		TI_A,  // A
		TI_B,  // B
		TI_C,  // C
		TI_D,  // D
		TI_E,  // E
		TI_F,  // F
		TI_G,  // G
		TI_H,  // H
		TI_I,  // I
		TI_J,  // J
		TI_K,  // K
		TI_L,  // L
		TI_M,  // M
		TI_N,  // N
		TI_O,  // O
		TI_P,  // P
		TI_Q,  // Q
		TI_R,  // R
		TI_S,  // S
		TI_T,  // T
		TI_U,  // U
		TI_V,  // V
		TI_W,  // W
		TI_X,  // X
		TI_Y,  // Y
		TI_Z,  // Z
		TI_ADDFCTN|TI_R,  // [
		TI_ADDFCTN|TI_Z,  // \ .
		TI_ADDFCTN|TI_T,  // ]
		TI_ADDSHIFT|TI_6,  // ^
		TI_ADDFCTN|TI_U,  // _
		TI_ADDSHIFT|TI_A,  // a
		TI_ADDSHIFT|TI_B,  // b
		TI_ADDSHIFT|TI_C,  // c
		TI_ADDSHIFT|TI_D,  // d
		TI_ADDSHIFT|TI_E,  // e
		TI_ADDSHIFT|TI_F,  // f
		TI_ADDSHIFT|TI_G,  // g
		TI_ADDSHIFT|TI_H,  // h
		TI_ADDSHIFT|TI_I,  // i
		TI_ADDSHIFT|TI_J,  // j
		TI_ADDSHIFT|TI_K,  // k
		TI_ADDSHIFT|TI_L,  // l
		TI_ADDSHIFT|TI_M,  // m
		TI_ADDSHIFT|TI_N,  // n
		TI_ADDSHIFT|TI_O,  // o
		TI_ADDSHIFT|TI_P,  // p
		TI_ADDSHIFT|TI_Q,  // q
		TI_ADDSHIFT|TI_R,  // r
		TI_ADDSHIFT|TI_S,  // s
		TI_ADDSHIFT|TI_T,  // t
		TI_ADDSHIFT|TI_U,  // u
		TI_ADDSHIFT|TI_V,  // v
		TI_ADDSHIFT|TI_W,  // w
		TI_ADDSHIFT|TI_X,  // x
		TI_ADDSHIFT|TI_Y,  // y
		TI_ADDSHIFT|TI_Z,  // z
		TI_ADDFCTN|TI_F,  // {
		TI_ADDFCTN|TI_A,  // |
		TI_ADDFCTN|TI_G,  // }
		TI_ADDFCTN|TI_W,  // ~
	};
	return ch >= 32 && ch < 127 ? map[ch-32] : 0;
}

/****************************************
 * CRU read/write                       *
 ****************************************/

static u32 sampled_timer_value = 0;

u8 cru_r(u16 bit)
{

	if (timer_mode && bit >= 1 && bit <= 14) {

		return (sampled_timer_value >> (14-bit)) & 1;
	}
	switch (bit) {
	case 0: return timer_mode;
	case 2: //debug_log("VDPST=%02X\n", vdp.reg[VDP_ST]);
		return !(vdp.reg[VDP_ST] & 0x80);
	case 3: //     = . , M N / fire1 fire2
	case 4: // space L K J H ;
	case 5: // enter O I U Y P
	case 6: //       9 8 7 6 0
	case 7: //  fctn 2 3 4 5 1
	case 8: // shift S D F G A
	case 9: //  ctrl W E R T Q
	case 10://       X C V B Z
		if (keyboard_row & 8) {
			// CRU 21 is set ALPHA-LOCK
			return bit == 7 ? 1^alpha_lock : 1;
		}
		return ((keyboard[keyboard_row] >> (bit-3)) & 1)^1; // active low

	default: fprintf(stderr, "TB %d not implemented\n", bit); break;
	}
	return 1;
}

void cru_w(u16 bit, u8 value)
{
	switch (bit) {
	case 0: // 0=normal 1=timer
		timer_mode = value & 1;
		if (timer_mode) sampled_timer_value = get_total_cpu_cycles() >> 5;
		debug_log("timer_mode=%d\n", timer_mode);
		break;
	case 1:
	case 2:
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
	case 8:

	case 12:
	case 13:
	case 14:
	case 15:
		// set interrupt mask for pins 1-15
		tms9901_int_mask = (tms9901_int_mask & ~(1<<bit)) | ((value & 1)<<bit);
		break;
	case 21:
		value ^= 1; // Set ALPHA-LOCK (invert it)
		// fall thru
	case 18: //keyboard_row = (keyboard_row & ~1) | (value & 1); break;
	case 19: //keyboard_row = (keyboard_row & ~2) | ((value & 1) << 1); break;
	case 20: //keyboard_row = (keyboard_row & ~4) | ((value & 1) << 2); break;
		undo_push(UNDO_KB, keyboard_row);
		keyboard_row &= ~(1 << (bit-18));
		keyboard_row |= (value & 1) << (bit-18);
		break;

	default:
		//fprintf(stderr, "%s %d not implemented at pc=%x\n", value ? "SBO" : "SBZ", bit, get_pc());
		break;
	case 0x1e00 >> 1: break;  // SAMS mapper access
	case 0x1e02 >> 1: break;  // SAMS mpping mode
	case 0x1e04 >> 1: break;  // SAMS 4MB mode?
	}
}




void unhandled(u16 pc, u16 op)
{
	debug_log("unhandled opcode %04x at pc %04x\n", op, pc);
	if (pc >= 0x8000 && pc < 0x8400) {
		debug_log("%04x\n", fast_ram[(pc>>1)&0x7f]);
	}
	int i;
	for (i = 0; i < 128; i++) {
		if ((i&7) == 0) debug_log("\n%04x:",0x8300+i*2);
		debug_log("%04x ", fast_ram[i]);
	}
	debug_log("\n");
	//exit(0);
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
#define FIFTH_SPRITE 0x40
#define SPRITE_COINC 0x20

static int config_sprites_per_line = SPRITES_PER_LINE;

enum {
	MODE_1_STANDARD = 0,
	MODE_2_BITMAP = 2,
	MODE_8_MULTICOLOR = 8,
	MODE_10_TEXT = 0x10,
	MODE_12_TEXT_BITMAP = 0x12,
	MODE_SPRITES = 0x20,
};

static void draw_char_patterns(
	uint32_t *pixels,
	unsigned int sy,
	unsigned char *scr,
	unsigned char mode,
	unsigned char *reg,
	unsigned char *ram,
	int bord,
	int len)
{
	uint32_t bg = palette[reg[7] & 0xf];
	uint32_t fg = palette[(reg[7] >> 4) & 0xf];
	unsigned char *col = ram + reg[3] * 0x40;
	unsigned char *pat = ram + (reg[4] & 0x7) * 0x800 + (sy & 7);

	if (mode == MODE_1_STANDARD) {
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
	} else if (mode & MODE_10_TEXT) {
		unsigned char i = len == 640 ? 80 : 40;
		// text mode(s)
		scr += (sy / 8) * i;
		for (int i = 0; i < bord/4; i++) {
			*pixels++ = bg;
		}
		if (mode == MODE_10_TEXT) {
			for (; i; i--) {
				unsigned char ch = *scr++;
				ch = pat[ch * 8];
				for (unsigned char j = 0x80; j != 2; j >>= 1)
					*pixels++ = ch & j ? fg : bg;
			}
		} else if (mode == MODE_12_TEXT_BITMAP) { // text bitmap
			unsigned int patmask = ((reg[4]&3)<<11)|(0x7ff);
			pat = ram + (reg[4] & 0x04) * 0x800 +
				(((sy / 64) * 2048) & patmask) + (sy & 7);
			for (; i; i--) {
				unsigned char ch = *scr++;
				ch = pat[ch * 8];
				for (unsigned char j = 0x80; j != 2; j >>= 1)
					*pixels++ = ch & j ? fg : bg;
			}
		} else { // illegal mode (40 col, 4px fg, 2px bg)
			for (; i; i--) {
				*pixels++ = fg;
				*pixels++ = fg;
				*pixels++ = fg;
				*pixels++ = fg;
				*pixels++ = bg;
				*pixels++ = bg;
			}
		}
		for (int i = 0; i < bord/4; i++) {
			*pixels++ = bg;
		}

	} else if (mode == MODE_2_BITMAP) {
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
			uint32_t
				fg = palette[c >> 4],
				bg = palette[c & 15];
			ch = pat[(ch & colmask) * 8];
			for (unsigned char j = 0x80; j; j >>= 1)
				*pixels++ = ch & j ? fg : bg;
		}
	} else if (mode == MODE_8_MULTICOLOR) {
		// multicolor 64x48 pixels
		pat -= (sy & 7); // adjust y offset
		pat += ((sy / 4) & 7);

		scr += (sy / 8) * 32;  // get row

		for (unsigned char i = 32; i; i--) {
			unsigned char
				ch = *scr++,
				c = pat[ch * 8];
			uint32_t
				fg = palette[c >> 4],
				bg = palette[c & 15];
			for (unsigned char j = 0; j < 4; j++)
				*pixels++ = fg;
			for (unsigned char j = 0; j < 4; j++)
				*pixels++ = bg;
		}

	} else if (mode == MODE_SPRITES) {
		// sprite patterns - debug only 
		unsigned char *sp = ram + (reg[6] & 0x7) * 0x800 // sprite pattern table
			+ (sy & 7);
		scr += (sy / 8) * 32;
		fg = palette[15];
		bg = palette[1];

		for (unsigned char i = 32; i; i--) {
			unsigned char ch = *scr++;
			ch = sp[ch * 8];
			for (unsigned char j = 0x80; j; j >>= 1)
				*pixels++ = ch & j ? fg : bg;
		}


	}
}


void vdp_line(unsigned int line,
		unsigned char* restrict reg,
		unsigned char* restrict ram)
{
	uint32_t *pixels;
	int len = 320;
	int bord = LFTBORD;
	uint32_t bg = palette[reg[7] & 0xf];
	uint32_t fg = palette[(reg[7] >> 4) & 0xf];

	palette[0] = palette[(reg[7] & 0xf) ?: 1];

	if ((reg[0] & 6) == 4 && (reg[1] & 0x18) == 0x10) {
		// 80-col text mode
		len *= 2;
		bord *= 2;
	}

	vdp_lock_texture(line, len, (void**)&pixels);

	if (line < TOPBORD || line >= TOPBORD+192 || (reg[1] & 0x40) == 0) {
		// draw border or blanking
		for (int i = 0; i < len; i++) {
			*pixels++ = bg;
		}
	} else {
		unsigned int sy = line - TOPBORD; // screen y, adjusted for border
		unsigned char *scr = ram + (reg[2] & 0xf) * 0x400;
		unsigned char mode = (reg[0] & 0x02) | (reg[1] & 0x18);

		// draw left border
		for (int i = 0; i < bord; i++) {
			*pixels++ = bg;
		}

		uint32_t *save_pixels = pixels;

		// draw graphics
		draw_char_patterns(pixels, sy, scr, mode, reg, ram, bord, len);
		pixels += len - 2 * bord;

		// draw right border
		for (int i = 0; i < bord; i++) {
			*pixels++ = bg;
		}
		pixels = save_pixels;

		// no sprites in text mode
		if (!(reg[1] & 0x10)) {
			unsigned char sp_size = (reg[1] & 2) ? 16 : 8;
			unsigned char sp_mag = sp_size << (reg[1] & 1); // magnified sprite size
			unsigned char *sl = ram + (reg[5] & 0x7f) * 0x80; // sprite list
			unsigned char *sp = ram + (reg[6] & 0x7) * 0x800; // sprite pattern table
			unsigned char coinc[256] = {0};

			// draw sprites  (TODO limit 4 sprites per line, and higher priority sprites)
			struct {
				unsigned char *p;  // Sprite pattern data
				unsigned char x;   // X-coordinate
				unsigned char f;   // Color and early clock bit
			} sprites[32]; // TODO Sprite limit hardcoded at 4
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
				if (sprite_count == SPRITES_PER_LINE && (reg[8] & FIFTH_SPRITE) == 0) {
					reg[8] &= 0xe0;
					reg[8] |= FIFTH_SPRITE + i;
				}
				if (sprite_count >= config_sprites_per_line)
					break;

				sprites[sprite_count].p = sp + (s * 8) + ((dy - (y+1)) >> (reg[1]&1));
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
	vdp_unlock_texture();
}

void redraw_vdp(void)
{
	int y;
	for (y = 0; y < 240; y++) {
		vdp_line(y, vdp.reg, vdp.ram);
	}
}

static void print_name_table(u8* reg, u8 *ram)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned char *line = ram + (reg[2]&0xf)*0x400;
	int offset = (reg[2]&0xf) == 0 && (reg[4]&0x7) == 0 ?
			0x60 : 0; // use 0 for no offset, 0x60 for XB
	int y, x, w = (reg[1] & 0x10) ? 40 : 32;
	for (y = 0; y < 24; y++) {
		for (x = 0; x < w; x++) {
			unsigned char c = *line++ - offset;
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


#ifndef USE_SDL

static uint32_t frame_buffer[320*240];

void vdp_lock_texture(int line, int len, void**pixels)
{
	*pixels = frame_buffer + line*320;
}
void vdp_unlock_texture(void)
{
}


void vdp_init(void)
{
}

void vdp_done(void)
{
}

char **test_argv = NULL;
int test_argc = 0;

int vdp_update(void)
{
	static int countdown = 0;
	static char *type_text = NULL;
	if (type_text) {
		if (type_text[0] == 0) {
			type_text = NULL;
		} else if (ti_key_pressed()) {
			reset_ti_keys();
			type_text++;
		} else {
			int k = char2key(type_text[0]);
			if (k&TI_ADDFCTN) set_key(TI_FCTN,1);
			if (k&TI_ADDCTRL) set_key(TI_CTRL,1);
			if (k&TI_ADDSHIFT) set_key(TI_SHIFT,1);
			set_key(k & 0x3f, 1);
		}
	} else if (countdown) {
		countdown--;
	} else if (test_argc > 0) {
		

	}

	static int frames = 0;
	if (frames == 82 || frames == 112) {
		set_key(TI_1, 1);
		//print_name_table(vdp.reg, vdp.ram);

	} else if (frames == 88 || frames == 116) {
		set_key(TI_1, 0);
	}
	frames++;
	return frames >= 600;
}

#define unused __attribute__((unused))


void vdp_text_pat(unused unsigned char *pat)
{
}

void vdp_text_window(const char *line, int w, int h, int x, int y, int highlight_line)
{
}

void vdp_set_fps(unused int mfps /* fps*1000 */)
{
}

void mute(unused int en)
{
}

int debug_window(void)
{
	return 0;
}
#endif






/****************************************
 * ROM/GROM loading                     *
 ****************************************/

static char *argv0_dir_name = NULL;

// Load a ROM cartridge, allocating memory into dest_ptr, size into size_ptr,
// and comparing expected size in *size_ptr if nonzero
// Returns 0 on success, -1 on error or insufficient read
static int load_rom(char *filename, u16 **dest_ptr, unsigned int *size_ptr)
{
	FILE *f = fopen(filename, "rb");
	u16 *dest = *dest_ptr;
	unsigned int i, size, buf_size = size_ptr ? *size_ptr : 0;

#ifdef COMPILED_ROMS
	for (i = 0; i < ARRAY_SIZE(comp_roms); i++) {
		if (strcmp(comp_roms[i].filename, filename) == 0) {
			*dest_ptr = (u16*) comp_roms[i].data;
			*size_ptr = comp_roms[i].len;
			return 0;
		}
	}
#endif
	if (!f && argv0_dir_name) {
		char fn[PATH_MAX];
		if (sprintf(fn, "%s/%s", argv0_dir_name, filename) > 0) {
			//printf("trying %s\n", fn);
			f = fopen(fn, "rb");
		}
	}
	if (!f) {
		//debug_log("Failed to open %s\n", filename);
		//fprintf(stderr, "Failed to open %s\n", filename);
		return -1; //exit(1);
	}
	if (fseek(f, 0, SEEK_END) < 0)
		perror("fseek SEEK_END");
	size = ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0)
		perror("fseek SEEK_SET");

	if (buf_size != 0 && size != buf_size) {
		fprintf(stderr, "ROM %s size expected %d, not %d\n", filename, buf_size, size);
		size = buf_size;
	}

	if (!dest || size > buf_size) {
		buf_size = (size + 0x1fff) & ~0x1fff; // round up to nearest 8k
		dest = my_realloc(dest, buf_size);
		*dest_ptr = dest;
		if (size_ptr) *size_ptr = buf_size;
	}

	for (i = 0; i < size; i += 2) {
		u8 buf[2];
		if (fread(buf, 1, 2, f) < 2) {
			debug_log("Failed to read ROM...\n");
			break;
		}
		// roms are stored big-endian
		*dest++ = buf[1] + (buf[0] << 8);
	}
	fclose(f);
	//printf("loaded ROM %d/%d\n", i, size);
	return i < size ? -1 : 0;
}

static int load_grom(char *filename, u8 **dest_ptr, unsigned int *size_ptr)
{
	unsigned int size = size_ptr ? *size_ptr : 0;

#ifdef COMPILED_ROMS
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(comp_roms); i++) {
		if (strcmp(comp_roms[i].filename, filename) == 0) {
			*dest_ptr = (u8*) comp_roms[i].data;
			*size_ptr = comp_roms[i].len;
			return 0;
		}
	}
#endif
	FILE *f = fopen(filename, "rb");
	if (!f && argv0_dir_name) {
		char fn[PATH_MAX];
		if (sprintf(fn, "%s/%s", argv0_dir_name, filename) > 0) {
			f = fopen(fn, "rb");
		}
	}
	if (!f) {
		//debug_log("Failed to open %s\n", filename);
		//fprintf(stderr, "Failed to open %s\n", filename);
		return -1;
	}
	if (fseek(f, 0, SEEK_END) < 0)
		perror("fseek SEEK_END");
	size = ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0)
		perror("fseek SEEK_SET");
	if (*size_ptr != 0 && size != *size_ptr) {
		fprintf(stderr, "GROM %s size expected %d, not %d\n", filename, *size_ptr, size);
	}

	if (!*dest_ptr || size < *size_ptr) {
		*size_ptr = size;
		*dest_ptr = my_realloc(*dest_ptr, *size_ptr);
	}
	if (*dest_ptr) {
		int n = fread(*dest_ptr, 1, *size_ptr, f);
		if (n < *size_ptr) {
			debug_log("Failed to read GROM...\n");
		}
	}
	fclose(f);
	return 0;
}


#ifdef ENABLE_DEBUGGER

/****************************************
 * Clipboard/text pasting function      *
 ****************************************/

static char *paste_str = NULL;
static int paste_idx = 0;
static int paste_old_fps = 0;
static int paste_kscan_address = 0x0478; // 99/4A ROM only
static int paste_delay = 0;

void paste_cancel(void)
{
	if (!paste_str) return;
	free(paste_str);
	paste_str = NULL;
	set_breakpoint(paste_kscan_address, -1, BREAKPOINT_DISABLE);
	vdp_set_fps(paste_old_fps);
}

static void paste_char(void)
{
	// Borrowed from Classic99 Mike Brent
	u8 b = fast_ram[0x74>>1] >> 8;

	if (paste_str == NULL) return;
	if (b == 0 || b == 5) {
		u8 c = paste_str[paste_idx++];
		if (c == 10) { // LF
			if (paste_idx >= 2 && paste_str[paste_idx-2] == 13) {
				// if prev char was CR then don't output this one
				paste_delay = 0;
				return;
			}
			c = 13; // LF to CR
		}
		if (paste_delay) { // this seems to fix pasting on LF-only systems
			paste_delay = 0;
			paste_idx--;
			return;
		}

		if(0)printf("b=%d  %04x %04x %d %c\n", b, fast_ram[0x74>>1],
				fast_ram[0x7c>>1],
				c, c);
		if (c == 0) {
			paste_cancel();
		} else if ((c >= 32 && c < 127) || c == 13) {
			paste_delay = (c == 13);
			u16 wp = get_wp();
			if (wp >= 0x8000 && wp < 0x8400) {
				wp &= 0xff;
				// R0 = charcode
				fast_ram[wp>>1] = c << 8;
				// R6 = status byte
				fast_ram[(wp>>1)+6] = /*fast_ram[0x7c>>1]|*/0x2000;
			} else {
				// TODO handle workspace in expansion ram
			}
		}
	}
}

void paste_text(char *text, int old_fps)
{
	// Set up a breakpoint in KSCAN to inject keys
	set_breakpoint(paste_kscan_address, -1, BREAKPOINT_PASTE);

	paste_str = my_strdup(text);
	paste_idx = 0;
	paste_old_fps = old_fps;
	//printf("pasting %s\n", paste_str);
	vdp_set_fps(0); // max speed
}




/****************************************
 * Debugger interface functions         *
 ****************************************/

#if 0

struct breakpoint {
	struct breakpoint *next; // linked list
	enum {
		BREAKPOINT,
		MEM_ACCESS,
		MEM_READ,
		MEM_WRITE,
		VDP_READ,
		VDP_WRITE,
		GROM_READ,
		GROM_WRITE,
		AMS_READ,
		AMS_WRITE,
		VDP_REG_CMP,
		WP_REG_CMP,
		MEM_BYTE_CMP,
		MEM_WORD_CMP,
		VDP_BYTE_CMP,
		AMS_BYTE_CMP,
	} type;
	u16 address;
	u16 address_end;
	u16 mask;  // used with CMP types
	u16 value; // used with CMP types
};
#endif

static struct {
	int address;
	int bank;
	int enabled;
} *breakpoint;
static int breakpoint_count = 0;
static int breakpoint_skip_address = -1; // for resuming after a breakpoint, or single-stepping

void set_break(int debug_state)
{
	debug_break = debug_state;
	if (debug_break != DEBUG_RUN) {
		// clear the keyboard buffer
		set_ui_key(0);
	}
}

int breakpoint_read(u16 address) // called from brk_r() before read
{
	int i;

	//printf("breakpoint address=%04X skip=%d debug_en=%d\n", address,
	//		breakpoint_skip_address, debug_en);
	if (breakpoint_skip_address != -1) {
		if (breakpoint_skip_address == address) {
			breakpoint_skip_address = -1;
			return 0;
		}
		breakpoint_skip_address = -1;
	}
	// don't break if single-stepping!
	if (debug_break >= DEBUG_SINGLE_STEP) {
		return 0;
	}

	// scan list of breakpoints to see if one is hit
	for (i = 0; i < breakpoint_count; i++) {
		if (address != breakpoint[i].address)
			continue;

		if (address >= 0x6000 && address < 0x8000 &&
		    breakpoint[i].bank != -1 && breakpoint[i].bank != cart_bank)
			continue; // wrong bank
		if (!breakpoint[i].enabled)
			continue; // not enabled

		if (breakpoint[i].enabled == BREAKPOINT_PASTE) {
			paste_char();
			return 0;
		}

		if (debug_en == 0)
			continue; // debugger not open, but could paste instead

		// breakpoint hit
		set_break(DEBUG_STOP);

		// skip it next time when the debugger resumes execution
		// FIXME what if the PC is changed before resuming
		breakpoint_skip_address = address;

		return 1; // memory read should return a BREAK opcode
	}
	//printf("didn't find a breakpoint for %04x bank %d\n", address, cart_bank);
	return 0;
}

int breakpoint_write(u16 address) // called from brk_w() after write
{
	// TODO scan list of breakpoints to see if one is hit

	return 0;
}

int breakpoint_index(u16 address, int bank)
{
	int i;
	for (i = 0; i < breakpoint_count; i++) {
		if (breakpoint[i].address == address) {
			if (bank == -1 || bank == breakpoint[i].bank) {
				return i;
			}
		}
	}
	return -1;
}

void remove_breakpoint(u16 address, int bank)
{
	int i = breakpoint_index(address, bank);
	if (i == -1) return;
	memmove(&breakpoint[i], &breakpoint[i+1], sizeof(*breakpoint)*(breakpoint_count-i));
	breakpoint_count--;
	// TODO could shrink breakpoint array
}

// set or toggle breakpoint, enable=-1 to toggle, otherwise set to enable
void set_breakpoint(u16 address, int bank, int enable)
{
	int i = breakpoint_index(address, bank);
	if (i == -1) {
		// create a new breakpoint
		i = breakpoint_count++;
		breakpoint = realloc(breakpoint, sizeof(*breakpoint) * breakpoint_count);
		breakpoint[i].address = address;
		breakpoint[i].bank = bank;
		breakpoint[i].enabled = enable == BREAKPOINT_TOGGLE ? 1 : enable;
	} else {
		breakpoint[i].enabled = enable == BREAKPOINT_TOGGLE ? !breakpoint[i].enabled : enable;
	}
	//printf("i=%d address=%x bank=%d enabled=%d\n", i, breakpoint[i].address, breakpoint[i].bank, breakpoint[i].enabled);
	cpu_reset_breakpoints();
	for (i = 0; i < breakpoint_count; i++) {
		if (breakpoint[i].enabled)
			cpu_set_breakpoint(breakpoint[i].address, 2/*bytes*/);
	}
}

int enum_breakpoint(int index, int *address, int *bank, int *enabled)
{
	if (index >= 0 && index < breakpoint_count) {
		*address = breakpoint[index].address;
		*bank = breakpoint[index].bank;
		*enabled = breakpoint[index].enabled;
		return 1;
	}
	return 0;
}

int get_breakpoint(int address, int bank)
{
	int i = breakpoint_index(address, bank);
	if (i == -1) return -1;
	return breakpoint[i].enabled;
}

#endif // ENABLE_DEBUGGER




// set cart name before doing a reset
void set_cart_name(char *name)
{
	free(cartridge_name);
	cartridge_name = my_strdup(name);
}

// for ui.c:listing_search() in the debugger
int get_cart_bank(void)
{
	return cart_bank;
}

void reset(void)
{
	cpu_reset();

	// reset VDP regs
	vdp.reg[0] = 0;
	vdp.reg[1] = 0;
	vdp.latch = 0;
	vdp.a = 0;
	vdp.y = 0;

	printf("initial PC=%04X WP=%04X ST=%04X\n", get_pc(), get_wp(), get_st());
	//debug_log("initial WP=%04X PC=%04X ST=%04X\n", get_wp(), get_pc(), get_st());

 	ga = 0xb5b5; // grom address
	grom_last = 0xaf;


#ifdef ENABLE_UNDO
	undo_head = 0; undo_tail = 0;
#endif
	cart_bank = 0;

	// TODO reload cartridge images too
	if (cartridge_name) {
		unsigned int len = strlen(cartridge_name);

		my_free(cart_rom);
		cart_rom = NULL;
		cart_rom_size = 0;

		my_free(cart_grom);
		cart_grom = NULL;
		cart_grom_size = 0;

		// optionally load ROM
		if (load_rom(cartridge_name, &cart_rom, &cart_rom_size) == 0) {
			// loaded success, try D rom
			if (tolower(cartridge_name[len-5]) == 'c' && cart_rom_size == 8192) {
				char *name = malloc(len + 1);
				memcpy(name, cartridge_name, len + 1);
				// if '*c.bin' TODO check for '*d.bin'
				name[len-5]++; // C to D
				cart_rom = realloc(cart_rom, 16384);
				cart_rom += 8192/2;
				load_rom(name, &cart_rom, &cart_rom_size);
				cart_rom -= 8192/2;
				cart_rom_size += 8192;
				free(name);
			}
		}

		if (cart_rom) {
			unsigned int banks = (cart_rom_size + 0x1fff) >> 13;

			// get next power of 2
			cart_bank_mask = banks>1 ? (1 << (32-__builtin_clz(banks-1))) - 1 : 0;
#if 0
			cart_bank_mask = banks-1;
			cart_bank_mask |= cart_bank_mask >> 1;
			cart_bank_mask |= cart_bank_mask >> 2;
			cart_bank_mask |= cart_bank_mask >> 4;
			cart_bank_mask |= cart_bank_mask >> 8;
			cart_bank_mask |= cart_bank_mask >> 16;
#endif

			printf("cart_bank_mask = 0x%x (size=%d banks=%d) page_size=%d\n",
				cart_bank_mask, cart_rom_size, banks, 256/*1<<MAP_SHIFT*/);
			set_cart_bank(0);
		}

		// optionally load GROM
		{
			char *name = malloc(len + 2);

			// try replace 8/9/C.bin with G.bin
			memcpy(name, cartridge_name, len + 1);
			name[len-5] = isupper(name[len-5]) ? 'G' : 'g';

			//printf("load grom %s\n", name);
		        if (load_grom(name, &cart_grom, &cart_grom_size) == -1) {
				// try insert G otherwise
				memcpy(name, cartridge_name, len + 1);
				memmove(name+len-3, name+len-4, 5); // move the .bin
				name[len-4] = isupper(name[len-5]) ? 'G' : 'g';

				//printf("load grom %s\n", name);
				load_grom(name, &cart_grom, &cart_grom_size);
			}
			free(name);
			//printf("grom=%p size=%u\n", cart_grom, cart_grom_size);
		}
#ifndef TEST
		// optionally load listing
		{
			char *name = malloc(len + 1);
			memcpy(name, cartridge_name, len + 1);
			// replace .bin with .lst
			name[len-3] ^= 'b' ^ 'l';
			name[len-2] ^= 'i' ^ 's';
			name[len-1] ^= 'n' ^ 't';
			//printf("load listing %s\n", name);
			load_listing(name, -1);
			free(name);
		}
#endif
	}
}


int vdp_update_or_menu(void)
{
	if (vdp_update() != 0)
		return -1; // quitting
#ifndef TEST
	if (keyboard[0] & (1 << TI_MENU)) {
		set_ui_key(0); // reset ui keys
		mute(1);
		if (main_menu() == -1)
			return -1; // quitting

#ifdef ENABLE_DEBUGGER
		if (debug_break == DEBUG_RUN)
			mute(0);
#else
		mute(0);
#endif
		set_key(TI_MENU, 0); // unset the key so don't retrigger menu
	}
#endif
	return 0;
}

#ifdef ENABLE_DEBUGGER

int debug_pattern_type = 0; // 0-2=pattern tables 3=sprite table
void update_debug_window(void)
{
	char reg[53*30] = { [0 ... 53*30-1]=32};
	u16 pc, wp;

	pc = get_pc();
	wp = get_wp();

	if(0) {sprintf((char*)reg,
	      "\n  VDP0: %02X  VDP: %04X      R0: %04X   R8:  %04X\n"
		"  VDP1: %02X  VDPST: %02X      R1: %04X   R9:  %04X\n"
		"  VDP2: %02X                 R2: %04X   R10: %04X\n"
		"  VDP3: %02X      PC: %04X   R3: %04X   R11: %04X\n"
		"  VDP4: %02X      WP: %04X   R4: %04X   R12: %04X\n"
		"  VDP5: %02X      ST: %04X   R5: %04X   R13: %04X\n"
		"  VDP6: %02X                 R6: %04X   R14: %04X\n"
		"  VDP7: %02X                 R7: %04X   R15: %04X\n"
		"  KB: %02X %02X %02X %02X %02X %02X %02X %02X\n\n"
		"\n                                          \n",
		vdp.reg[0], vdp.a, safe_r(wp), safe_r(wp+16),
		vdp.reg[1], vdp.reg[VDP_ST], safe_r(wp+2), safe_r(wp+18),
		vdp.reg[2], safe_r(wp+4), safe_r(wp+20),
		vdp.reg[3], get_pc(), safe_r(wp+6), safe_r(wp+22),
		vdp.reg[4], wp, safe_r(wp+8), safe_r(wp+24),
		vdp.reg[5], get_st(), safe_r(wp+10), safe_r(wp+26),
		vdp.reg[6], safe_r(wp+12), safe_r(wp+28),
		vdp.reg[7], safe_r(wp+14), safe_r(wp+30),
		keyboard[0], keyboard[1], keyboard[2], keyboard[3],
		keyboard[4], keyboard[5], keyboard[6], keyboard[7]);
	vdp_text_window(reg, 53, 30, 320, 0, -1);
	} else {
		sprintf((char*)reg,
	      "\n PC: %04X    R0: %04X\n"
		" WP: %04X    R1: %04X\n"
		" ST: %04X    R2: %04X\n"
		"             R3: %04X\n"
		" VDP0: %02X    R4: %04X\n"
		" VDP1: %02X    R5: %04X\n"
		" VDP2: %02X    R6: %04X\n"
		" VDP3: %02X    R7: %04X\n"
		" VDP4: %02X    R8: %04X\n"
		" VDP5: %02X    R9: %04X\n"
		" VDP6: %02X   R10: %04X\n"
		" VDP7: %02X   R11: %04X\n"
		" VDP: %04X  R12: %04X\n"
		" VDPST: %02X  R13: %04X\n"
		"  Y: %-3d    R14: %04X\n"
		" BANK: %-4d R15: %04X\n"
		"            \n"
		" KB: ROW: %d\n"
		"%02X %02X %02X %02X %02X %02X %02X %02X\n\n",
		pc,         safe_r(wp),
		wp,         safe_r(wp+2),
		get_st(),   safe_r(wp+4),
		            safe_r(wp+6),
		vdp.reg[0], safe_r(wp+8),
		vdp.reg[1], safe_r(wp+10),
		vdp.reg[2], safe_r(wp+12),
		vdp.reg[3], safe_r(wp+14),
		vdp.reg[4], safe_r(wp+16),
		vdp.reg[5], safe_r(wp+18),
		vdp.reg[6], safe_r(wp+20),
		vdp.reg[7], safe_r(wp+22),
		vdp.a,      safe_r(wp+24),
		vdp.reg[VDP_ST], safe_r(wp+26),
		vdp.y,	    safe_r(wp+28),
		cart_bank,  safe_r(wp+30),
		keyboard_row,
		keyboard[0], keyboard[1], keyboard[2], keyboard[3],
		keyboard[4], keyboard[5], keyboard[6], keyboard[7]);
		vdp_text_window(reg, 23,30, 0,240, -1);
	}
	// show fast ram state
	int i, n = 0;
	for (i = 0; i < 128; i+=8) {
		n += sprintf(reg+n, "  %04X:  %04X %04X %04X %04X  %04X %04X %04X %04X\n",
			0x8300+i*2, fast_ram[i], fast_ram[i+1], fast_ram[i+2], fast_ram[i+3],
			fast_ram[i+4], fast_ram[i+5], fast_ram[i+6], fast_ram[i+7]);
	}
	reg[n] = 0;
	vdp_text_window(reg, 53,30, 322,0, -1);

	// draw char patterns 
	{
		unsigned char scr[32*24], save_r4 = vdp.reg[4];
		void *pixels;
		unsigned char mode = (vdp.reg[0] & 0x02) | (vdp.reg[1] & 0x18);
		unsigned char sp_size = 0;
		int i, y_offset = debug_pattern_type * 64;

		if (debug_pattern_type == 3) {
			y_offset = 0;
			mode = MODE_SPRITES;
			sp_size = (vdp.reg[1] & 2) >> 1; // 0=8pix 1=16pix
		}

		for (i = 0; i < 32*24; i++) {
			// linear, otherwise 02/13 layout for 16x16 sprites
			scr[i] = !sp_size ? i : ((i & 0xc0)|((i&31)<<1)|((i>>5)&1));
		}

		for (i = 0; i < 8*8; i++) {
			vdp_lock_debug_texture(i+16*8, 640, &pixels);
			draw_char_patterns(((uint32_t*)pixels) + 320+32,
				i+y_offset, scr, mode, vdp.reg, vdp.ram, 32, 320);
			vdp_unlock_debug_texture();
		}
		vdp_text_window(&"1\n2\n3\nS\n"[debug_pattern_type*2], 1,1, 322+3*6,16*8, -1);
	}
}


static void emu_check_undo(void)
{
	struct state s0, s1, s2;

	save_state(&s0);
	do {
		u16 save_pc = get_pc();

		single_step();
		save_state(&s1);
		undo_pop();
		save_state(&s2);
		if (memcmp(&s0, &s2, sizeof(struct state)) != 0) {
			FILE *f;
			printf("undo failed at pc=%04x\n", save_pc);
			f = fopen("bulwip_undo0.txt", "w");
			print_state(f, &s0);
			//fwrite(&s0, sizeof(struct state), 1, f);
			fclose(f);
			f = fopen("bulwip_undo1.txt", "w");
			print_state(f, &s2);
			//fwrite(&s2, sizeof(struct state), 1, f);
			fclose(f);
			exit(0);
		}
		single_step(); // redo
		save_state(&s0);
		if (memcmp(&s0, &s1, sizeof(struct state)) != 0) {
			FILE *f;
			printf("redo failed at pc=%04x\n", save_pc);
			f = fopen("bulwip_undo0.txt", "w");
			print_state(f, &s0);
			//fwrite(&s0, sizeof(struct state), 1, f);
			fclose(f);
			f = fopen("bulwip_undo1.txt", "w");
			print_state(f, &s2);
			//fwrite(&s2, sizeof(struct state), 1, f);
			fclose(f);
			exit(0);
		}
	} while (add_cyc(0) < 0);
}
#endif // ENABLE_DEBUGGER



int main(int argc, char *argv[])
{
#ifdef LOG_DISASM
	log = fopen("/tmp/bulwip.log","w");
#endif
	//log = fopen("/dev/null","w");
	//log = fopen("/tmp/cpu.log","w");
	//if (!log) log = stderr;
	//if (!log) log = fopen("NUL","w");

	mem_init();

	disasmf = log;

	// Get app dir name for when loading roms
	char *slash = strrchr(argv[0], '/');
	if (slash) {
		int len = slash - argv[0];
		argv0_dir_name = malloc(len + 1);
		memcpy(argv0_dir_name, argv[0], len);
		argv0_dir_name[len] = 0;
		//fprintf(stderr, "dir_name = %s\n", argv0_dir_name);
	}

	rom_size = 8192;
	load_rom("994arom.bin", &rom, &rom_size);
	grom_size = 24576;
	load_grom("994agrom.bin", &grom, &grom_size);
#ifndef TEST
	load_listing("994arom.lst", -1);
#endif
	if (!rom || !grom) {
		fprintf(stderr, "Failed to load ROM/GROM files: %s %s\n",
			rom ? "" : "994arom.bin",
			grom ? "" : "994agrom.bin");
		exit(0);
	}

	// Give GROM char patterns for debugger
	vdp_text_pat(grom + 0x06B4 - 32*7);

	if (argc > 1) {
		set_cart_name(argv[1]); // will get loaded on reset
		//load_rom(argv[1], &cart_rom, &cart_rom_size);
		argv++;
	} else {
		//load_rom("../phantis/phantisc.bin", &cart_rom, &cart_rom_size);
		//load_rom("cputestc.bin", &cart_rom, &cart_rom_size);
		//load_rom("../wordit/wordit8.bin", &cart_rom, &cart_rom_size);
#ifdef TEST
		//load_rom("cputestc.bin", &cart_rom, &cart_rom_size);
		//load_rom("test/mbtest.bin", &cart_rom, &cart_rom_size);
		load_rom("mbtest.bin", &cart_rom, &cart_rom_size);
#endif
	}

	//vdp_window_scale(4);
	vdp_init();

	reset();

	//add_breakpoint(BREAKPOINT, 0x3aa, 0, 0, 0);
#ifdef TEST
	//debug_en = 1; debug_break = DEBUG_STOP;
	test_argv = argv;
	test_argc = argc;
#endif

#ifndef TEST
	//load_listing("romsrc/ROM-4A.lst", 0);
	//load_listing("../bnp/bnp.lst", 0);
	//load_listing("fcmd/FCMD.listing", 0);

#endif

#ifdef ENABLE_GIF
	GifBegin(&gif, "bulwip.gif", /*width*/320, /*height*/240, /*delay*/2, /*bitDepth*/4, /*dither*/false);
#endif
	int lines_per_frame = 262; // NTSC=262 PAL=313

	do {
#ifdef ENABLE_DEBUGGER
		if (debug_en) {
			if (debug_window() == -1) break;
		}
#endif

		// render one frame
		do {
			// render one scanline
			if (vdp.y < 240) {
				undo_push(UNDO_VDPST, vdp.reg[VDP_ST]);
				vdp_line(vdp.y, vdp.reg, vdp.ram);
			} else if (vdp.y == 246) {
				undo_push(UNDO_VDPST, vdp.reg[VDP_ST]);
				vdp.reg[VDP_ST] |= 0x80;  // set F in VDP status
				if (vdp.reg[1] & 0x20) // check IE
					interrupt(1);  // VDP interrupt
			}
			undo_push(UNDO_VDPY, vdp.y);
			if (++vdp.y == lines_per_frame) {
				vdp.y = 0;
			}

			total_cycles_busy = total_cycles + CYCLES_PER_LINE;
			total_cycles = total_cycles_busy;
			add_cyc(-CYCLES_PER_LINE);
			total_cycles_busy = 0;
#ifdef ENABLE_DEBUGGER
			if (debug_break == DEBUG_SINGLE_STEP) {
				single_step();
				set_break(DEBUG_STOP);
				break;
			}
#endif
			emu(); // emulate until cycle counter goes positive
			//emu_check_undo();
			// a breakpoint will change debug_break variable

		} while (vdp.y != 0
#ifdef ENABLE_DEBUGGER
			&& (debug_break == DEBUG_RUN || debug_break == DEBUG_FRAME_STEP)
#endif
			);
#ifdef ENABLE_GIF
		GifWriteFrame(&gif, (u8*)frame_buffer, /*width*/320, /*height*/240, /*delay*/2, /*bitDepth*/4, /*dither*/false);
#endif
	} while (vdp_update_or_menu() == 0);

#ifdef ENABLE_GIF
	GifEnd(&gif);
#endif
	//debug_log("%d\n", total_cycles);
	vdp_done();
#ifdef TEST
	print_name_table(vdp.reg, vdp.ram);
#endif

	if (log) fclose(log);
	if (disasmf && disasmf != log) fclose(disasmf);
}
