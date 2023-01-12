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
#define _POSIX_C_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "cpu.h"

//#define TRACE_GROM
//#define TRACE_VDP
//#define TRACE_CPU
//#define COMPILED_ROMS

#ifdef COMPILED_ROMS
#include "994arom.h"
#include "994agrom.h"
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

static char asm_text[80] = "";

int print_asm(const char *fmt, ...)
{
	{
		va_list ap;
		int len = strlen(asm_text);
		va_start(ap, fmt);
		vsnprintf(asm_text+len, sizeof(asm_text)-len, fmt, ap);
		va_end(ap);
	}
	if (!disasmf) return 0;
	{
		va_list ap;
		va_start(ap, fmt);
		int ret = vfprintf(disasmf, fmt, ap);
		va_end(ap);
		return ret;
	}
}


#ifndef TEST
#define USE_SDL
#endif
#ifdef USE_SDL
// sdl.h
extern void snd_w(unsigned char byte);
extern void vdp_init(void);
extern void vdp_done(void);
extern int vdp_update(void);
extern void vdp_line(unsigned int y, u8* restrict reg, u8* restrict ram);
extern void vdp_text_window(const unsigned char *line, int w, int h, int x, int y, int highlight_line);
extern void vdp_text_pat(unsigned char *pat);
#endif

#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))


static u16 tms9901_int_mask = 0; // 9901 interrupt mask
static u8 trace = 1; // disassembly trace flag
int debug_en = 0;
int debug_break = 0;

static void dbg_break(int en) { debug_en = 1; debug_break = en; }

static u16 fast_ram[128] = {}; // 256 bytes at 8000-80ff,repeated at 8100,8200,8300

static u16 *ram = NULL; // 32k RAM or SAMS
static unsigned int ram_size = 0; // in bytes

static u16 *cart_ram = NULL;
static unsigned int cart_size = 0;
static u16 cart_bank_mask = 0;

static u16 *rom = NULL;
static unsigned int rom_size = 0;

static u8 *grom = NULL;
static unsigned int grom_size = 0;
static u8 grom_latch = 0, grom_last = 0x00;
static u16 ga; // grom address


#define VDP_RAM_SIZE (16*1024)
static struct {
	u8 ram[VDP_RAM_SIZE];
	u16 a; // address
	u8 latch;
	u8 reg[9]; // vdp status is [8]
} vdp = {
	.ram = {},
	.a = 0,
	.latch = 0,
	.reg = {0,0,0,0,1,0,0,0,0},
};


static void *safe_alloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}


/******************************************
 * 8000-9FFF  RAM & memory mapped devices *
 ******************************************/

static u16 safe_ram8_r(u16 address)
{
	switch ((address >> 10) & 3) {
	case 0: // 0x8000
		// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
		add_cyc(2);
		//debug_log("RAM read %04X = %04X\n", address & 0xfffe, fast_ram[(address & 0xfe) >> 1]);
		return fast_ram[(address & 0xfe) >> 1];
	}
	return 0;
}

static u16 ram8_r(u16 address)
{
	switch ((address >> 10) & 3) {
	case 0: // 0x8000
		// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
		add_cyc(2);
		//debug_log("RAM read %04X = %04X\n", address & 0xfffe, fast_ram[(address & 0xfe) >> 1]);
		return fast_ram[(address & 0xfe) >> 1];
	case 1: // 0x8400
		// sound chip (illegal to read?)
		add_cyc(6);
		return 0;
	case 2: // 0x8800
		add_cyc(6);
		vdp.latch = 0;
		if (address == 0x8800) {
			// 8800   VDP RAM read data register
			return vdp.ram[vdp.a++ & 0x3fff] << 8;
		} else if (address == 0x8802) {
			// 8802   VDP RAM read status register
			u16 value = vdp.reg[8] << 8;
			//debug_log("VDP_STATUS=%0x\n", vdp.reg[8]);
			vdp.reg[8] &= ~0xE0; // clear interrupt flags
			interrupt(-1); // deassert INTREQ
			return value;
		}
		break;
	case 3: // 0x8c00
		add_cyc(6);
		if (address == 0x8C00) {
			// 8C00   VDP RAM write data register
			return 0;
		} else if (address == 0x8C02) {
			// 8C02   VDP RAM write address register
			return 0;
		}
		break;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static void ram8_w(u16 address, u16 value)
{
	switch ((address >> 10) & 3) {
	case 0: // 0x8000
		// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
		add_cyc(2);
		fast_ram[(address & 0xfe) >> 1] = value;
//		if (address == 0x8380) {
//			debug_log("RAM write %04X = %04X\n", address & 0xfffe, value);
//			dump_regs();
//		}
		return;
	case 1: // 0x8400
		// sound chip
		add_cyc(34);
		// TODO
#ifdef USE_SDL
		snd_w(value >> 8);
#endif
		return;
	case 2: // 0x8800
		if (address == 0x8800) {
			// 8800   VDP RAM read data register
		} else if (address == 0x8802) {
			// 8802   VDP RAM read status register
		}
		break;
	case 3: // 0x8c00
		if (address == 0x8C00) {
			// 8C00   VDP RAM write data register
			//debug_log("VDP write %04X = %02X\n", vdp.a, value >> 8);
			vdp.ram[vdp.a] = value >> 8;
			vdp.a = (vdp.a + 1) & 0x3fff; // wraps at 16K
			return;
		} else if (address == 0x8C02) {
			// 8C02   VDP RAM write address register
			vdp.latch ^= 1;
			if (vdp.latch) {
				// first low byte 
				vdp.a = value >> 8;
			} else {
				// second high byte
				if (value & 0x8000) {
					// register write
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
		break;
	}
	debug_log("unhandled RAM write %04X %04X at PC=%04X\n", address, value, get_pc());
}

static u16 ram9_r(u16 address)
{
	switch ((address >> 10) & 3) {
	case 0: // 0x9000
		// speech
		add_cyc(54);
		// TODO
		return 0;

	case 1: // 0x9400
		break;

	// Console GROMs will map at GROM addresses >0000-5FFF in any base
	case 2: // 0x9800
		if ((address & 3) == 0) {
			// grom read data
			u16 value = grom_last << 8;
#ifdef TRACE_GROM
			debug_log("%04X GROM read %04X %02X\n", get_pc(), ga, grom_last);
#endif
			// pc=4e8  GET NEXT BYTE FROM GAME ROM
			// pc=556  LOAD LOOP ADDRESS
			// pc=55a   2nd byte?
			// pc=77c  GET NEXT BYTE FROM GROM (2ND BYTE)

			grom_last = grom[ga];

			// increment and wrap around
			ga = (ga & 0xe000) | ((ga+1) & 0x1fff);

			add_cyc(25);
			grom_latch = 0;
			return value;
		} else if ((address & 3) == 2) {
			// grom read address (plus one) (first low byte, then high)
			u16 value = (ga & 0xff00);
			add_cyc(19);
			grom_latch = 0;
			ga = (ga << 8) | (ga & 0x00ff);
			//debug_log("%04X GROM addr read %04X\n", get_pc(), ga);
			return value;
		}
		break;
	case 3: // 0x9c00
		if ((address & 3) == 0 || (address & 3) == 2) {
			// grom write data / address
			add_cyc(25);
			return 0;
		}
		break;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static u16 safe_ram9_r(u16 address)
{
	switch ((address >> 10) & 3) {
	case 0: // 0x9000
		// speech
		return 0;

	case 1: // 0x9400
		break;

	// Console GROMs will map at GROM addresses >0000-5FFF in any base
	case 2: // 0x9800
		if ((address & 3) == 0) {
			// grom read data
			u16 value = grom_last << 8;
			//debug_log("%04X GROM read %04X %02X\n", get_pc(), ga, grom_last);

			// pc=4e8  GET NEXT BYTE FROM GAME ROM
			// pc=556  LOAD LOOP ADDRESS
			// pc=55a   2nd byte?
			// pc=77c  GET NEXT BYTE FROM GROM (2ND BYTE)

			//grom_last = grom[ga];

			// increment and wrap around
			//ga = (ga & 0xe000) | ((ga+1) & 0x1fff);

			//add_cyc(25);
			//grom_latch = 0;
			return value;
		} else if ((address & 3) == 2) {
			// grom read address (plus one) (first low byte, then high)
			u16 value = (ga & 0xff00);
			//add_cyc(19);
			//grom_latch = 0;
			//ga = (ga << 8) | (ga & 0x00ff);
			//debug_log("%04X GROM addr read %04X\n", get_pc(), ga);
			return value;
		}
		break;
	case 3: // 0x9c00
		if ((address & 3) == 0 || (address & 3) == 2) {
			// grom write data / address
			return 0;
		}
		break;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}


static void ram9_w(u16 address, u16 value)
{
	switch ((address >> 10) & 3) {
	case 0: // 0x9000
		// speech
		add_cyc(70);
		// TODO
		return;
	case 1: // 0x9400
		break;
	case 2: // 0x9800
		if ((address & 3) == 0 || (address & 3) == 2) {
			// grom read data/address
			return;
		}
		break;
	case 3: // 0x9c00
		if ((address & 3) == 0) {
			// grom write data
			//debug_log("%04X GROM write %04X %02X\n", get_pc(), ga, value);
			if (ga >= 0x6000) {
				grom[ga] = value >> 8;
				// increment and wrap around
				ga = (ga & 0xe000) | ((ga+1) & 0x1fff);
			}
			add_cyc(28);
			return;
		} else if ((address & 3) == 2) {
			// grom write address
			ga = ((ga << 8) & 0xff00) | (value >> 8);

			grom_latch ^= 1;
			if (grom_latch) {
				// first
				add_cyc(21);
			} else {
				// second
				add_cyc(27);
				grom_last = grom[ga];
				// increment and wrap around
				ga = (ga & 0xe000) | ((ga+1) & 0x1fff);
			}
#ifdef TRACE_GROM
			debug_log("%04X GROM write address %04X %02X\n", get_pc(), ga, value>>8);
#endif
			return;
		}
		break;
	}
	debug_log("unhandled RAM write %04X %04X at PC=%04X\n", address, value, get_pc());
}

/****************************************
 * 0000-1FFF  ROM                       *
 ****************************************/

static u16 rom_r(u16 address)
{
	add_cyc(2);
	//printf("%04X => %04X\n", rom[address >> 1]);
	return rom[address>>1];
}

static void rom_w(u16 address, u16 value)
{
	// rom not writable
	debug_log("ROM write %04X %04X at pc=%x\n", address, value, get_pc());
	//printf("ROM write %04X %04X at pc=%x\n", address, value, get_pc());
	//trace = 1;
	add_cyc(2);
}

/****************************************
 * 4000-5FFF  DSR ROM (paged by CRU?)   *
 ****************************************/

static u16 dsr_rom_r(u16 address)
{
	add_cyc(6); // 4 extra cycles for multiplexer
	// TODO each DSR peripheral may do different things based on CRU
	return 0;
}

static void dsr_rom_w(u16 address, u16 value)
{
	add_cyc(6); // 4 extra cycles for multiplexer
	// TODO each DSR peripheral may do different things based on CRU
}


/****************************************
 * 6000-7FFF  Cartridge ROM/RAM         *
 ****************************************/

static void cart_rom_w(u16 address, u16 value)
{
	u16 bank = (address >> 1) & cart_bank_mask;
	add_cyc(6); // 4 extra cycles for multiplexer


	// 8K banking
	u16 *base = cart_ram + bank * 4096/*words per 8KB bank*/;
	//int i;
	//for (i = 0; i < (1 << (13-MAP_SHIFT)); i++) {
		//map[i + (0x6000 >> MAP_SHIFT)].mem = base + (i << (MAP_SHIFT-1));
	//}
	change_mapping(0x6000, 0x2000, base);

	//cart_ram[address>>1] = value;
	// TODO bank switch or RAM
	//debug_log("Cartridge ROM write %04X %04X\n", address, value);
	//exit(0);
}




/****************************************
 * 2000-3FFF,A000-FFFF  Expansion RAM   *
 ****************************************/


static u16 zero_r(u16 address)
{
	add_cyc(6); // 4 extra cycles for multiplexer
	return 0;
}

static void zero_w(u16 address, u16 value)
{
	add_cyc(6); // 4 extra cycles for multiplexer
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
	set_mapping(0, rom_r, zero_w, NULL);
	set_mapping(1, rom_r, zero_w, NULL);
	
	// low memory expansion 2000-3fff
	set_mapping(2, map_r, map_w, ram+(0x0000/2));
	set_mapping(3, map_r, map_w, ram+(0x1000/2));

	// DSR ROM 4000-5fff
	set_mapping(4, zero_r, zero_w, NULL);
	set_mapping(5, zero_r, zero_w, NULL);
	
	// cartridge ROM 6000-7fff
	set_mapping(6, map_r, map_w, NULL);
	set_mapping(7, map_r, map_w, NULL);
	
	// memory mapped devices 8000-9fff
	set_mapping_safe(8, ram8_r, safe_ram8_r, ram8_w, NULL);
	set_mapping_safe(9, ram9_r, safe_ram9_r, ram9_w, NULL);

	// high memory expansion A000-ffff
	set_mapping(10, map_r, map_w, ram+(0x2000/2));
	set_mapping(11, map_r, map_w, ram+(0x3000/2));
	set_mapping(12, map_r, map_w, ram+(0x4000/2));
	set_mapping(13, map_r, map_w, ram+(0x5000/2));
	set_mapping(14, map_r, map_w, ram+(0x6000/2));
	set_mapping(15, map_r, map_w, ram+(0x7000/2));

	// instruction aquisition func
	//iaq_func = mem_r;
}


/****************************************
 * CRU read/write                       *
 ****************************************/

u8 keyboard[8] = {};
static u8 keyboard_row = 0;
static u8 timer_mode = 0;
static unsigned int total_cycles = 0;

static void set_key(int k, int val)
{
	int row = ((k) >> 3) & 7, col = (k) & 7; \
	keyboard[row] = (keyboard[row] & ~(1<<col)) | ((val) << col); \
}


u8 cru_r(u16 bit)
{
	if (timer_mode && bit >= 1 && bit <= 14) {

		return ((total_cycles+add_cyc(0)) >> (14-bit)) & 1;
	}
	switch (bit) {
	case 0: return timer_mode;
	case 2: //debug_log("VDPST=%02X\n", vdp.reg[8]);
		return (vdp.reg[8] & 0x80);
	case 3: //     = . , M N / fire1 fire2
	case 4: // space L K J H ;
	case 5: // enter O I U Y P
	case 6: //       9 8 7 6 0
	case 7: //  fctn 2 3 4 5 1
	case 8: // shift S D F G A
	case 9: //  ctrl W E R T Q
	case 10://       X C V B Z
		//if ((keyboard[keyboard_row] >> (bit-3)) & 1)
		//printf("keyboard %d %d %d\n", keyboard_row, bit-3,
		//	(keyboard[keyboard_row] >> (bit-3)) & 1);

		return ((keyboard[keyboard_row] >> (bit-3)) & 1) ^ 1;

	default: fprintf(stderr, "TB %d not implemented\n", bit); break;
	}
	return 0;
}

void cru_w(u16 bit, u8 value)
{
	switch (bit) {
	case 0: // 0=normal 1=timer
		timer_mode = value & 1;
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
	case 18: //keyboard_row = (keyboard_row & ~1) | (value & 1); break;
	case 19: //keyboard_row = (keyboard_row & ~2) | ((value & 1) << 1); break;
	case 20: //keyboard_row = (keyboard_row & ~4) | ((value & 1) << 2); break;
		keyboard_row &= ~(1 << (bit-18));
		keyboard_row |= (value & 1) << (bit-18);
		break;

	case 21: break;

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
 * ROM/GROM loading                     *
 ****************************************/

static char *argv0_dir_name = NULL;

static void load_rom(char *filename, u16 **dest_ptr, unsigned int *size_ptr)
{
	FILE *f = fopen(filename, "rb");
	u16 *dest = *dest_ptr;
	unsigned int size = size_ptr ? *size_ptr : 0;
	unsigned int i;

	if (!f && argv0_dir_name) {
		char fn[PATH_MAX];
		if (sprintf(fn, "%s/%s", argv0_dir_name, filename) > 0) {
			f = fopen(fn, "rb");
		}
	}
	if (!f) {
		debug_log("Failed to open %s\n", filename);
		fprintf(stderr, "Failed to open %s\n", filename);
		exit(1);
	}
	if (size == 0) {
		if (fseek(f, 0, SEEK_END) < 0)
			perror("fseek SEEK_END");
		size = ftell(f);
		if (fseek(f, 0, SEEK_SET) < 0)
			perror("fseek SEEK_SET");
		if (size_ptr)
			*size_ptr = size;
	}
	if (!dest) {
		dest = safe_alloc(size);
		*dest_ptr = dest;
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
	debug_log("loaded ROM %d/%d\n", i, size);
	if (i < size) {
		exit(0);
	}
}

static void load_grom(char *filename, u16 addr)
{
	FILE *f = fopen(filename, "rb");
	if (!f && argv0_dir_name) {
		char fn[PATH_MAX];
		if (sprintf(fn, "%s/%s", argv0_dir_name, filename) > 0) {
			f = fopen(fn, "rb");
		}
	}
	if (!f) {
		debug_log("Failed to open %s\n", filename);
		fprintf(stderr, "Failed to open %s\n", filename);
		exit(1);
	}
	while (!feof(f)) {
		//debug_log("GROM addr %x\n", addr);
		if ((unsigned int)addr + 0x2000 > grom_size) {
			grom_size = addr + 0x2000;
			grom = realloc(grom, grom_size);
		}
		if (fread(grom + addr, 1, 0x2000, f) == 0)
			break;
		addr += 0x2000;
	}
	fclose(f);
}

static char* load_file(char *filename, unsigned int *out_size)
{
	FILE *f = fopen(filename, "rb");
	unsigned int size = 0;
	char *p = NULL;
	if (!f) return NULL;
	while (!feof(f)) {
		unsigned int chunk = 4096;
		p = realloc(p, size + chunk + 1);
		size_t n = fread(p + size, 1, chunk, f);
		size += n < chunk ? n : chunk;
	}
	p[size] = 0;
	if (out_size) *out_size = size;
	return p;
}


static void print_name_table(u8* reg, u8 *ram)
{
	unsigned char *line = ram + (reg[2]&0xf)*0x400;
	int y, x, w = (reg[1] & 0x10) ? 40 : 32;
	for (y = 0; y < 24; y++) {
		for (x = 0; x < w; x++) {
			printf("%c", *line++);
		}
		printf("\n");
	}

}


#ifndef USE_SDL
static void vdp_init(void)
{
}

static void vdp_done(void)
{
}

static int vdp_update(void)
{
	static int frames = 0;
	if (frames == 82 || frames == 112) {
		set_key(12, 1);
		//print_name_table(vdp.reg, vdp.ram);

	} else if (frames == 88 || frames == 116) {
		set_key(12, 0);
	}
	frames++;
	return frames >= 600;
}

#define unused __attribute__((unused))

static void vdp_line(unused int i, unused u8 *reg, unused u8 *ram)
{
}

static void vdp_text_pat(unused unsigned char *pat)
{
}

static void vdp_text_window(const unsigned char *line, int w, int h, int x, int y, int highlight_line)
{
}

void mute(int en)
{
}
#endif


#if 0
/****************************************
 * Debugger interface functions         *
 ****************************************/

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

static struct breakpoint* breakpoint_list = NULL;

static int debugging = 0;

static u16 check_break_func(u16 address)
{
	if (!debugging) {
		struct breakpoint *bp = breakpoint_list;
		debugging = 1;
		while (bp) {
			if (bp->address <= address && address <= bp->address_end && bp->type == BREAKPOINT) {
				int i;
				for (i = 0; i < 10; i++) {
					disasm(get_pc());
					single_step();
					//print_regs();
				}
				
				address = get_pc(); // important after single stepping, need to return opcode at pc
				break;
			}
			bp = bp->next;
		}
		debugging = 0;
	}
	return safe_r(address);
}



void add_breakpoint(int type, u16 address, u16 address_end, u16 mask, u16 value)
{
	struct breakpoint *bp;

	bp = safe_alloc(sizeof(*bp));
	bp->next = breakpoint_list;
	bp->type = type;
	bp->address = address;
	bp->address_end = address_end ?: address;
	bp->mask = mask ?: 0xffff;
	bp->value = value;

	//if (type == BREAKPOINT) {
		//iaq_func = check_break_func;
	//}

	breakpoint_list = bp;
}
#endif




void reset(void)
{
	cpu_reset();

	// reset VDP regs
	vdp.reg[0] = 0;
	vdp.reg[1] = 0;

	debug_log("initial WP=%04X PC=%04X ST=%04X\n", get_wp(), get_pc(), get_st());

 	ga = 0xb5b5; // grom address
	grom_last = 0xaf;

	// TODO reload cartridge images too
}


unsigned int next_line(const char *lst, unsigned int len, unsigned int a)
{
	// skip non-newline chars until end of line
	while (a < len && lst[a] != '\r' && lst[a] != '\n')
		a++;
	// skip newline chars until start of line
	while (a < len && (lst[a] =='\r' || lst[a] == '\n'))
		a++;
	return a;
}

unsigned int prev_line(const char *lst, unsigned int len, unsigned int a)
{
	// skip newline chars until end of prev line
	while (a > 0 && (lst[a-1] == '\r' || lst[a-1] == '\n'))
		a--;
	// skip non-newline chars until start of line
	while (a > 0 && lst[a-1] != '\r' && lst[a-1] != '\n')
		a--;
	return a;
}

int get_line_pc(const char *lst, unsigned int a)
{
	unsigned int i = a, n = 0;
	u16 pc = 0;
	// a line containing a PC may only have spaces a line number preceding it
	while (i-a < 5 && lst[i] == ' ') i++; // skip any leading spaces
	while (i-a < 5 && lst[i] >= '0' && lst[i] <= '9') i++; // skip the line number
	while (i-a < 6 && lst[i] == ' ') i++; // skip spaces between line number and pc
	if (i-a < 5 || i-a > 6) return -1; // PC must be at 5 or 6
	for (n = 0; n < 4; n++) {
		char c = lst[i++];
		if (c >= '0' && c <= '9')
			pc = pc*16 + c - '0';
		else if (c >= 'A' && c <= 'F')
			pc = pc*16 + c - 'A' + 10;
		else
			return -1;
	}
	return pc;
}

// search the listing for the address pc
static unsigned int listing_search(const char *lst, unsigned int len, u16 pc)
{
	unsigned int a = 0, b = len, c;
	int a_pc = -1, b_pc = -1, c_pc = -1;

	// we're treating negative or zero PC as invalid (listing may have 0000 for EQU PC)
	while (a_pc <= 0) {
		if (a >= len) return 0;
		a_pc = get_line_pc(lst, a);
		//printf("a=%d pc=%04X\n", a, a_pc);
		if (a_pc > 0) break;
		a = next_line(lst, len, a);
	}
	while (b_pc <= 0) {
		if (b == 0) return 0;
		b_pc = get_line_pc(lst, b);
		//printf("b=%d pc=%04X\n", b, b_pc);
		if (b_pc > 0) break;
		b = prev_line(lst, len, b);
	}
	//printf("a=%04X b=%04X c=%04X\n", a_pc, b_pc, c_pc);
	while (1) {
		if (a_pc == pc) return a;
		if (b_pc == pc) return b;
		if (a == b) {
			//printf("a=%04X b=%04x\n", a_pc, b_pc);
			return 0;
		}
		c = (a+b)/2; // FIXME: could overflow
		c_pc = -1;
		while (c_pc <= 0) {
			c = prev_line(lst, len, c);
			if (c == 0) return 0;
			c_pc = get_line_pc(lst, c);
		}
		if (c_pc == pc) return c;
		//printf("a=%04X b=%04X c=%04X\n", a_pc, b_pc, c_pc);
		if (c_pc < pc) {
			if (a == c) {
				while (c_pc < pc) {
					c = next_line(lst, len, c);
					if (c >= len) return 0;
					c_pc = get_line_pc(lst, c);
				}
				return c;
			}
			a = c;
			a_pc = c_pc;
		} else {
			b = c;
			b_pc = c_pc;
		}
	}
	return 0;
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

int main(int argc, char *argv[])
{
	//log = fopen("/tmp/bulwip.log","w");
	//log = fopen("/dev/null","w");
	//log = fopen("/tmp/cpu.log","w");
	//if (!log) log = stderr;
	//if (!log) log = fopen("NUL","w");


	mem_init();

	disasmf = log;

	// Get app dir name for when loading roms
	char *slash = strrchr(argv[0], '/');
	if (slash) {
		argv0_dir_name = malloc(slash - argv[0] + 1);
		strcpy(argv0_dir_name, argv[0]);
		//fprintf(stderr, "dir_name = %s\n", argv0_dir_name);
	}

	//load_rom("99test2.bin_0000", 0);
	//load_rom("eptest.bin_0000", 0);
#ifdef COMPILED_ROMS
	rom = safe_alloc(rom994a_len);
	rom_size = rom994a_len;
	for (unsigned int i = 0; i < rom994a_len; i += 2) {
		// rom994a is big-endian
		rom[i/2] = ((u16)rom994a[i]<<8) | rom994a[i+1];
	}
	//printf("rom=%p size=%u %04X %04X %04X %04X\n", rom, rom_size, rom[0], rom[1], rom[2], rom[3]);
	grom = safe_alloc(grom994a_len);
	grom_size = grom994a_len;
	memcpy(grom, grom994a, grom994a_len);
	//printf("grom=%p size=%u %02X %02X %02X %02X\n", grom, grom_size, grom[0], grom[1], grom[2], grom[3]);
#else
	load_rom("994arom.bin", &rom, &rom_size);
	load_grom("994agrom.bin", 0x0000);
#endif

	// Give GROM char patterns for debugger
	vdp_text_pat(grom + 0x06B4 - 32*7);

	if (argc > 1) {
		load_rom(argv[1], &cart_ram, &cart_size);
	} else {
		//load_rom("../phantis/phantisc.bin", &cart_ram, &cart_size);
		//load_rom("cputestc.bin", &cart_ram, &cart_size);
		//load_rom("../wordit/wordit8.bin", &cart_ram, &cart_size);
#ifdef TEST
		//load_rom("cputestc.bin", &cart_ram, &cart_size);
		load_rom("test/mbtest.bin", &cart_ram, &cart_size);
#endif
	}

	if (cart_ram) {
		unsigned int banks = (cart_size + 0x1fff) >> 13;

		set_mapping(6, map_r, cart_rom_w, cart_ram);
		set_mapping(7, map_r, zero_w, cart_ram + 2048/*words*/);

		cart_bank_mask = banks ? (1 << (32-__builtin_clz(banks))) - 1 : 0;
		if (0)printf("cart_bank_mask = 0x%x  (size=%d banks=%d) page_size=%d\n",
			cart_bank_mask, cart_size, banks, 256/*1<<MAP_SHIFT*/);
	}

	vdp_init();

	reset();
	printf("initial PC=%04X WP=%04X ST=%04X\n", get_pc(), get_wp(), get_st());

	//add_breakpoint(BREAKPOINT, 0x3aa, 0, 0, 0);
#ifndef TEST
	debug_en = 1; debug_break = 1;
#endif

	unsigned int romsrc_len = 0;
	char *romsrc = load_file("romsrc/ROM-4A.lst", &romsrc_len);
	//printf("romsrc=%p len=%u\n", romsrc, romsrc_len);

	int lines_per_frame = 262; // NTSC=262 PAL=313
	while (vdp_update() == 0) {
		if (debug_en) {
			unsigned char reg[53*30] = { [0 ... 53*30-1]=32};
			u16 pc, wp;

			debug_refresh:
			pc = get_pc();
			wp = get_wp();
			memset(asm_text, 0, sizeof(asm_text));
			disasm(pc);
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
				"%s\n                                          \n",
				vdp.reg[0], vdp.a, safe_r(wp), safe_r(wp+16),
				vdp.reg[1], vdp.reg[8], safe_r(wp+2), safe_r(wp+18),
				vdp.reg[2], safe_r(wp+4), safe_r(wp+20),
				vdp.reg[3], get_pc(), safe_r(wp+6), safe_r(wp+22),
				vdp.reg[4], wp, safe_r(wp+8), safe_r(wp+24),
				vdp.reg[5], get_st(), safe_r(wp+10), safe_r(wp+26),
				vdp.reg[6], safe_r(wp+12), safe_r(wp+28),
				vdp.reg[7], safe_r(wp+14), safe_r(wp+30),
				keyboard[0], keyboard[1], keyboard[2], keyboard[3],
				keyboard[4], keyboard[5], keyboard[6], keyboard[7],
				asm_text);
			vdp_text_window(reg, 53, 30, 320, 0, -1);
			} else {
				sprintf((char*)reg,
			      "\n  VDP0: %02X      PC: %04X\n"
				"  VDP1: %02X      WP: %04X\n"
				"  VDP2: %02X      ST: %04X\n"
				"  VDP3: %02X              \n"
				"  VDP4: %02X      R 0: %04X\n"
				"  VDP5: %02X      R 1: %04X\n"
				"  VDP6: %02X      R 2: %04X\n"
				"  VDP7: %02X      R 3: %04X\n"
				" VDP: %04X      R 4: %04X\n"
				" VDPST: %02X      R 5: %04X\n"
				"                R 6: %04X\n"
				"                R 7: %04X\n"
				"                R 8: %04X\n"
				"                R 9: %04X\n"
				"                R10: %04X\n"
				"                R11: %04X\n"
				"                R12: %04X\n"
				"                R13: %04X\n"
				"                R14: %04X\n"
				"                R15: %04X\n"
				" KB:\n"
				" %02X %02X %02X %02X %02X %02X %02X %02X\n\n"
				"%s\n                                          \n",
				vdp.reg[0], pc,
				vdp.reg[1], wp,
				vdp.reg[2], get_st(),
				vdp.reg[3], 
				vdp.reg[4], safe_r(wp),
				vdp.reg[5], safe_r(wp+2),
				vdp.reg[6], safe_r(wp+4),
				vdp.reg[7], safe_r(wp+6),
				vdp.a,      safe_r(wp+8),
				vdp.reg[8], safe_r(wp+10),
					    safe_r(wp+12),
					    safe_r(wp+14),
					    safe_r(wp+16),
					    safe_r(wp+18),
					    safe_r(wp+20),
					    safe_r(wp+22),
					    safe_r(wp+24),
					    safe_r(wp+26),
					    safe_r(wp+28),
					    safe_r(wp+30),
				keyboard[0], keyboard[1], keyboard[2], keyboard[3],
				keyboard[4], keyboard[5], keyboard[6], keyboard[7],
				asm_text);
				vdp_text_window(reg, 26, 30, 0, 240, -1);
			}

			if (romsrc) {
				unsigned int o = listing_search(romsrc, romsrc_len, pc);
				if (o) {
					int line = 0;
					while (o != 0 && line < 14) {
						o = prev_line(romsrc, romsrc_len, o);
						line++;
					}

					//printf("pc=%04X o=%d\n", pc, o);
					vdp_text_window(romsrc+o, 80, 30, 640-80*6, 240, line);
				}
			}

			extern void mute(int en);
			if (debug_break) mute(1); // turn off sounds while stopped
			while (debug_break) {
				if (debug_break == 2) {
					// single-step
					debug_break = 1;
					cpu_break(1);
					//printf("%d\n", next_cyc);
					emu();
					cpu_break(0);
					goto debug_refresh;
				}
				if (vdp_update() != 0)
					goto done;
			}
			mute(0);
		}

		// render one frame
		for (int i = 0; i < lines_per_frame; i++) {
			if (i < 240) {
				vdp_line(i, vdp.reg, vdp.ram);
			} else if (i == 246) {
				vdp.reg[8] |= 0x80;  // set F in VDP status
				if (vdp.reg[1] & 0x20) // check IE
					interrupt(1);  // VDP interrupt
			}
			total_cycles += CYCLES_PER_LINE;
			add_cyc(-CYCLES_PER_LINE);
			emu();
		}
	}
	done:
	//debug_log("%d\n", total_cycles);
	vdp_done();
#ifdef TEST
	print_name_table(vdp.reg, vdp.ram);
#endif

	if (log) fclose(log);
	if (disasmf && disasmf != log) fclose(disasmf);
}
