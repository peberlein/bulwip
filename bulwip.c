/*
 *  bulwip.c - TI 99/4A emulator
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
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>


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
static u16 sams_bank[16] = {
	0x000,0x000, // >0000,>1000
	0x000,0x100, // >2000,>3000
	0x000,0x000, // >4000,>5000
	0x000,0x000, // >6000,>7000
	0x000,0x000, // >8000,>9000
	0x200,0x300, // >A000,>B000
	0x400,0x500, // >C000,>D000
	0x600,0x700};// >E000,>F000

char *cartridge_name = NULL;
static u16 *cart_rom = NULL;
static unsigned int cart_rom_size = 0;
static u8 *cart_grom = NULL;
static unsigned int cart_grom_size = 0;
static u16 cart_bank_mask = 0;
static u16 cart_bank = 0; // up to 512MB cart size
static bool cart_ram_mode = 0;
static bool cart_gram_mode = 0;

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
	unsigned int ret = total_cycles + add_cyc(0);
	if (ret == 0) {
		printf("%s: ret=%u total_cycles=%u\n", __func__, ret, total_cycles);
	}
	// otherwise combine 'total_cycles' and current cycle counter
	return ret;
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
		return vdp_read_data() << 8;
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
		return vdp_read_status() << 8;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static u16 vdp_8800_safe_r(u16 address)
{
	if (address == 0x8800) {
		// 8800   VDP RAM read data register
		return vdp_read_data_safe() << 8;
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
		return vdp_read_status_safe() << 8;
	}
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
		vdp_write_data(value >> 8);
		return;
	} else if (address == 0x8C02) {
		// 8C02   VDP RAM write address register
		undo_push(UNDO_VDPA, vdp.a);
		undo_push(UNDO_VDPL, vdp.latch);
		vdp_write_addr(value >> 8);
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
	// increment and wrap around, stay in the same GROM "bank"
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
	static int once = 1;

	if (cart_ram_mode) {
		// 4K banking  >6xxx ROM  >7xxx RAM
		// writes to >60xx change ROM bank
		// writes to >68xx change RAM bank

		u16 offset = (bank & 0x400) << 2; // 0=ROM 4096=RAM
		cart_bank = bank & cart_bank_mask;
		u16 *base = cart_rom + cart_bank * 4096/*words per 8KB bank*/;

		change_mapping(0x6000 + offset, 0x1000, base + offset);
	} else {
		// 8K banking
		if (bank > cart_bank_mask && once) {
			once = 0;
			printf("Warning: bank %x > %x mask pc=%04x r11=%04x\n", bank, cart_bank_mask, get_pc(), safe_r(get_wp()+22));
		}
		cart_bank = bank & cart_bank_mask;
		u16 *base = cart_rom + cart_bank * 4096/*words per 8KB bank*/;
		//printf("%s: address=%04X bank=%d\n", __func__, bank, cart_bank);
		change_mapping(0x6000, 0x2000, base);
	}
}

static void cart_rom_w(u16 address, u16 value)
{
	undo_push(UNDO_CB, cart_bank);
	set_cart_bank((address >> 1) & 0xfff);
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


// SAMS expansion, be sure to read
// https://www.unige.ch/medecine/nouspikel/ti99/superams.htm

// SAMS mapper registers, page# in MSB
// >4004  >2000-2FFF
// >4006  >3000-3FFF
// >4014  >A000-AFFF
// >4016  >B000-BFFF
// >4018  >C000-CFFF
// >401A  >D000-DFFF
// >401C  >E000-EFFF
// >401E  >F000-FFFF

#define SAMS_PAGE_SIZE 4096
#if 1
// 1MB SAMS mapping
#define SAMS_PAGE(n) (sams_bank[n]>>8)
#else
// 16MB SAMS mapping (FIXME - doesn't work with AMSTEST-4)
#define SAMS_PAGE(n) ((sams_bank[n]>>8)|((sams_bank[n]&0xf)<<8))
#endif


static int sams_transparent = 1;
static void sams_map(int n)
{
	unsigned int word_offset = SAMS_PAGE(n) * SAMS_PAGE_SIZE / 2;
	printf("%s: n=%x page=%d trans=%d\n", __func__, n, SAMS_PAGE(n), sams_transparent);
	change_mapping(n * SAMS_PAGE_SIZE, SAMS_PAGE_SIZE, ram + word_offset);
}

static void sams_mode(int mapping)
{
	printf("%s: mode=%d\n", __func__, mapping);
	sams_transparent = !mapping;
	if (mapping) {
		// mapping based on registers
		sams_map(0x2); sams_map(0x3); // >4004, >4006
		sams_map(0xa); sams_map(0xb); // >4014, >4016
		sams_map(0xc); sams_map(0xd); // >4018, >401A
		sams_map(0xe); sams_map(0xf); // >401C, >401E
	} else {
		// transparent mode: page 2 @ >2000, page 3 @ >3000, etc.
		change_mapping(0x2000, 0x2000, ram + 0x2000/2);
		change_mapping(0xa000, 0x6000, ram + 0xa000/2);
	}
}

// SAMS powers up with transparent mode enabled - to allow 
// 32K compatibility without mapping registers being initialized.
// however the emulator starts with 32K mapping 0@>2000 and >2000@>A000
// and SAMS transparent mode is >2000@>2000 and >A000@>A000

static void sams_init(void)
{
	if (get_pc() < 0x2000) return;
	printf("Initializing SAMS, PC=%04X\n", get_pc());

	// increase to 64K so that transparent mode can work
	ram_size = 0x10000;
	ram = my_realloc(ram, ram_size);

	// move the 32K data to keep the same layout in 64K
	memmove(ram+0xa000/2, ram+0x2000/2, 0x6000);
	memmove(ram+0x2000/2, ram+0x0000/2, 0x2000);
	// clear the rest: >0000->1fff, >4000->9ffff
	memset(ram+0x0000/2, 0, 0x2000);
	memset(ram+0x4000/2, 0, 0x6000);
	sams_mode(!sams_transparent); // update mappings
}

static void sams_4000_w(u16 address, u16 value)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	address &= ~1;
	if (address >= 0x4000 && address <= 0x401e) {
		int n = (address - 0x4000) / 2;
		sams_bank[n] = value;
		printf("sams reg[%x] = %04x\n", n, value);

		// don't map pages in >0000->1FFF,>4000->9FFF
		if ((1 << n) & 0x3f3) return;
		if (sams_transparent) return;

		unsigned int page = SAMS_PAGE(n);
		unsigned int page_end = (page+1) * SAMS_PAGE_SIZE;

		if (page_end > ram_size) {
			ram_size = page_end;
			ram = my_realloc(ram, ram_size);
			//printf("RAM increased to %d bytes %p\n", ram_size, ram);
			sams_mode(1); // reload register mappings
		}

		//printf("SAMS map >%04X, page >%02X\n", n * SAMS_PAGE_SIZE, page);
		sams_map(n);
	}
}

static u16 sams_4000_r(u16 address)
{
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	address &= ~1;
	if (address >= 0x4000 && address <= 0x401e) {
		int n = (address - 0x4000) / 2;
		return sams_bank[n];
	}
	return 0;
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
		// row 0 1 2 3 4 5 6     7
	case 3: //     = . , M N / fire1 fire2
	case 4: // space L K J H ; left  left
	case 5: // enter O I U Y P right right
	case 6: //       9 8 7 6 0 down  down
	case 7: //  fctn 2 3 4 5 1 up    up
	case 8: // shift S D F G A
	case 9: //  ctrl W E R T Q
	case 10://       X C V B Z
		if (keyboard_row & 8) {
			// CRU 21 is set ALPHA-LOCK
			return bit == 7 ? 1^alpha_lock : 1;
		}
		if ((keyboard_row & 7) >= 6) {
			// See: https://forums.atariage.com/topic/365610-keyboardjoystick-conflict/
			// Joysticks are disabled by keys on the same lines
			u8 rows = keyboard[0] | keyboard[1] | keyboard[2] |
				  keyboard[3] | keyboard[4] | keyboard[5];
			if ((rows >> (bit-3)) & 1)
				return 1; // not pressed
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

	case 0x1e00 >> 1:  // SAMS mapper access value: 0=enable 1=disable
		value ^= 1;
		printf("SAMS access %s\n", value ? "disabled" : "enabled");
		set_mapping(0x4000, 0x1000,
			value ? zero_r : sams_4000_r,
			value ? zero_w : sams_4000_w,
			NULL);
		break;
	case 0x1e02 >> 1:  // SAMS mapping mode 1=mapping 0=transparent
		printf("SAMS mode %s\n", value ? "mapping" : "transparent");
		if (value && ram_size < 0x10000)
			sams_init();
		sams_mode(value);
		break;
	case 0x1e04 >> 1:  // SAMS 4MB mode?
		printf("SAMS 4MB? %d\n", value);
		break;
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


void vdp_text_pat(unused u8 *pat)
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
		printf("size=%d buf_size=%d\n", size, buf_size);
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

	{
		static int once = 1;
		if (once) {
			once = 0;
			// reset VDP regs only the first time
			vdp_reset();
		}
	}


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
				u16 *rom2 = NULL;
				unsigned int rom2_size = 0;

				// copy name and increment last letter in filename
				memcpy(name, cartridge_name, len + 1);
				name[len-5]++; // C to D

				// attempt loading D rom
				if (load_rom(name, &rom2, &rom2_size) == 0) {
					cart_rom_size = 16384;
					cart_rom = realloc(cart_rom, cart_rom_size);
					memcpy(cart_rom + 8192/2, rom2, rom2_size < 8192 ? rom2_size : 8192);
					free(rom2);
				}
				free(name);
			}
		}

		if (cart_rom) {
			unsigned int banks = (cart_rom_size + 0x1fff) >> 13;

			// get next power of 2
			cart_bank_mask = banks>1 ? (1 << (32-__builtin_clz(banks-1))) - 1 : 0;

			printf("cart_bank_mask = 0x%x (size=%d banks=%d) page_size=%d mode=%c\n",
				cart_bank_mask, cart_rom_size, banks, 256/*1<<MAP_SHIFT*/,
				cart_rom[3] ?: ' ');
			cart_ram_mode = cart_rom[3] == 'R' || cart_rom[3] == 'X';
			cart_gram_mode = cart_rom[3] == 'G' || cart_rom[3] == 'X';

			if (cart_ram_mode) {
				set_mapping(0x6000, 0x1000, map_r, cart_rom_w, NULL);
				set_mapping(0x7000, 0x1000, map_r, map_w, NULL);
				set_cart_bank(0); // init ROM bank
				set_cart_bank(0x400);  // init RAM bank
			} else {
				// cartridge ROM 6000-7fff
				set_mapping(0x6000, 0x2000, map_r, cart_rom_w, NULL);
				set_cart_bank(0);
			}
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
		u8 scr[32*24];
#ifdef ENABLE_F18A
		u8 save_r27 = vdp.reg[27], save_r28 = vdp.reg[28];
		vdp.reg[27] = 0; vdp.reg[28] = 0; // clear pixel scroll regs
#endif
		u32 *pixels;
		u8 mode = (vdp.reg[0] & 0x02) | (vdp.reg[1] & 0x18);
		u8 sp_size = 0;
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
			vdp_lock_debug_texture(i+16*8, 640, (void**)&pixels);
			draw_char_patterns(pixels + 320+32,
				i+y_offset, scr, mode, vdp.reg, vdp.ram, 32, 320);
			vdp_unlock_debug_texture();
		}
		vdp_text_window(&"1\n2\n3\nS\n"[debug_pattern_type*2], 1,1, 322+3*6,16*8, -1);
#ifdef ENABLE_F18A
		vdp.reg[27] = save_r27;
		vdp.reg[28] = save_r28;
#endif
	}
	{	// draw palette
		int i,j;
		for (j = 0; j < 16; j++) {
			u32 *pixels;
			vdp_lock_debug_texture(j+25*8, 640, (void**)&pixels);
			for (i = 0; i < 32*8; i++) {
#ifdef ENABLE_F18A
				if (!vdp.locked)
					pixels[320+32+i] = pal_rgb((j&8)*4 + i/8);
				else
#endif
				pixels[320+32+i] = pal_rgb(i/16);
				//vdp_text_clear(320+32+(i&31)*8, 16*8+8*8+8+(i&32)/4, 2,1, pal_rgb(i));
			}
			vdp_unlock_debug_texture();
		}
	}

}

#ifdef ENABLE_UNDO
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
#endif
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
#ifdef ENABLE_F18A
			gpu();
#endif

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
