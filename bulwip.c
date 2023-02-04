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
//#define _POSIX_C_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>



#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "cpu.h"

#ifndef TEST
#define USE_SDL
#endif


//#define TRACE_GROM
//#define TRACE_VDP
//#define TRACE_CPU
//#define COMPILED_ROMS

#ifdef TEST
// use compiled roms to avoid I/O
#define COMPILED_ROMS
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



static u16 tms9901_int_mask = 0; // 9901 interrupt mask
static u8 trace = 1; // disassembly trace flag
int debug_en = 0;
int debug_break = 0;
int config_crt_filter = 0;

static void dbg_break(int en) { debug_en = 1; debug_break = en; }

static u16 fast_ram[128] = {}; // 256 bytes at 8000-80ff,repeated at 8100,8200,8300

static u16 *ram = NULL; // 32k RAM or SAMS
static unsigned int ram_size = 0; // in bytes

static u16 *cart_rom = NULL;
static unsigned int cart_rom_size = 0;
static u8 *cart_grom = NULL;
static unsigned int cart_grom_size = 0;
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
	u8 y; // scanline counter
} vdp = {
	.ram = {},
	.a = 0,
	.latch = 0,
	.reg = {0,0,0,0,1,0,0,0,0},
	.y = 0,
};

char *cartridge_name = NULL;




/******************************************
 * 8000-83FF  fast RAM                    *
 ******************************************/

static u16 ram_8300_r(u16 address)
{
	// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
	add_cyc(2);
	//debug_log("RAM read %04X = %04X\n", address & 0xfffe, fast_ram[(address & 0xfe) >> 1]);
	return fast_ram[(address & 0xfe) >> 1];
}

static void ram_8300_w(u16 address, u16 value)
{
	// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
	add_cyc(2);
	fast_ram[(address & 0xfe) >> 1] = value;
//		if (address == 0x8380) {
//			debug_log("RAM write %04X = %04X\n", address & 0xfffe, value);
//			dump_regs();
//		}
}



static u16 sound_8400_r(u16 address)
{
	add_cyc(6);
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
		add_cyc(6);
	}
}



static u16 vdp_8800_r(u16 address)
{
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
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}

static u16 vdp_8800_safe_r(u16 address)
{
	add_cyc(6);
	vdp.latch = 0;
	if (address == 0x8800) {
		// 8800   VDP RAM read data register
		return vdp.ram[vdp.a & 0x3fff] << 8;
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
		return vdp.reg[8] << 8;
	}
	debug_log("unhandled RAM read %04X at PC=%04X\n", address, get_pc());
	return 0;
}


static void vdp_8800_w(u16 address, u16 value)
{
	add_cyc(6);
	if (address == 0x8800) {
		// 8800   VDP RAM read data register
	} else if (address == 0x8802) {
		// 8802   VDP RAM read status register
	}
}


static u16 vdp_8c00_r(u16 address)
{
	add_cyc(6);
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
	add_cyc(6);
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
}


static u16 speech_9000_r(u16 address)
{
	if (address == 0x9000) {
		// speech
		add_cyc(54);
		// TODO
		return 0;
	}
	add_cyc(6);
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
	add_cyc(6);
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
	if ((address & 3) == 0) {
		// grom read data
#ifdef TRACE_GROM
		debug_log("%04X GROM read %04X %02X\n", get_pc(), ga, grom_last);
#endif
		// pc=4e8  GET NEXT BYTE FROM GAME ROM
		// pc=556  LOAD LOOP ADDRESS
		// pc=55a   2nd byte?
		// pc=77c  GET NEXT BYTE FROM GROM (2ND BYTE)

		u16 value = grom_last << 8;
		grom_read();
		grom_address_increment();

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
		ga = ((ga << 8) & 0xff00) | (value >> 8);

		grom_latch ^= 1;
		if (grom_latch) {
			// first
			add_cyc(21);
		} else {
			// second
			add_cyc(27);

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

static u16 cart_bank = 0;

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
	set_cart_bank(address >> 1);
	add_cyc(6); // 4 extra cycles for multiplexer
	//debug_log("Cartridge ROM write %04X %04X\n", address, value);
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
	set_mapping(0x0000, 0x2000, rom_r, zero_w, NULL);

	// low memory expansion 2000-3fff
	set_mapping(0x2000, 0x2000, map_r, map_w, ram+(0x0000/2));

	// DSR ROM 4000-5fff
	set_mapping(0x4000, 0x2000, zero_r, zero_w, NULL);

	// cartridge ROM 6000-7fff
	set_mapping(0x6000, 0x2000, map_r, cart_rom_w, NULL);

	// memory mapped devices 8000-9fff
	set_mapping(0x8000, 0x400, ram_8300_r, ram_8300_w, NULL);
	set_mapping(0x8400, 0x400, sound_8400_r, sound_8400_w, NULL);
	set_mapping(0x8800, 0x400, vdp_8800_r, vdp_8800_w, NULL);
	set_mapping(0x8c00, 0x400, vdp_8c00_r, vdp_8c00_w, NULL);
	set_mapping(0x9000, 0x400, speech_9000_r, speech_9000_w, NULL);
	set_mapping(0x9400, 0x400, zero_r, zero_w, NULL);
	set_mapping_safe(0x9800, 0x400, grom_9800_r, safe_grom_9800_r, zero_w, NULL);
	set_mapping(0x9c00, 0x400, zero_r, grom_9c00_w, NULL);

	// high memory expansion A000-ffff
	set_mapping(0xa000, 0x6000, map_r, map_w, ram+(0x2000/2));

	// instruction aquisition func
	//iaq_func = mem_r;
}


/****************************************
 * Keyboard                             *
 ****************************************/

// 0-7 unshifted keys 8-15 shifted keys 16-23 function keys
u8 keyboard[8] = {0}, keyboard_down[8] = {0};
static u8 keyboard_row = 0;
static u8 timer_mode = 0;
static unsigned int total_cycles = 0;

void set_key(int key, int val)
{
	int row = (key >> 3) & 7, col = key & 7, mask = val << col;
	keyboard[row] = (keyboard[row] & ~(1 << col)) | mask;
	keyboard_down[row] |= mask;
}

// returns a single keydown enum from TI_*
int wait_key(void)
{
	int i;
	do {
		for (i = 0; i < ARRAY_SIZE(keyboard_down); i++) {
			u8 b = keyboard_down[i];
			if (b == 0) continue;
			// get only one key
			b = __builtin_ctz(b);
			keyboard_down[i] &= ~(1 << b);
			if (keyboard[0] & (1<<TI_FCTN))
				b |= TI_ADDFCTN;
			else if (keyboard[0] & (1<<TI_SHIFT))
				b |= TI_ADDSHIFT;
			return i*8+b;
		}
	} while (vdp_update() == 0);
	return -1; // Window closed
}

int test_key(int key)
{
	int row = (key >> 3) & 7, col = key & 7, mask = 1 << col;
	if (keyboard_down[row] & mask) {
		//printf("row=%d mask=%d key=%02x\n", row, mask, 
		if ((key & 0xc0) == TI_ADDSHIFT) {
			if ((keyboard[0] & (1 << TI_SHIFT)) == 0) return 0;
			mask |= TI_SHIFT;
		} else if ((key & 0xc0) == TI_ADDFCTN) {
			if ((keyboard[0] & (1 << TI_FCTN)) == 0) return 0;
			mask |= TI_FCTN;
		}
		keyboard_down[row] ^= mask; // toggle it off
		return 1;
	}
	return 0;
}


/****************************************
 * CRU read/write                       *
 ****************************************/

u8 cru_r(u16 bit)
{
	if (timer_mode && bit >= 1 && bit <= 14) {

		return ((total_cycles+add_cyc(0)) >> (14-bit)) & 1;
	}
	switch (bit) {
	case 0: return timer_mode;
	case 2: //debug_log("VDPST=%02X\n", vdp.reg[8]);
		return !(vdp.reg[8] & 0x80);
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
			return bit != 7;
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
	case 18: //keyboard_row = (keyboard_row & ~1) | (value & 1); break;
	case 19: //keyboard_row = (keyboard_row & ~2) | ((value & 1) << 1); break;
	case 20: //keyboard_row = (keyboard_row & ~4) | ((value & 1) << 2); break;
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
 * ROM/GROM loading                     *
 ****************************************/

static char *argv0_dir_name = NULL;

static int load_rom(char *filename, u16 **dest_ptr, unsigned int *size_ptr, unsigned int base)
{
	FILE *f = fopen(filename, "rb");
	u16 *dest = *dest_ptr;
	unsigned int size = size_ptr ? *size_ptr : 0;
	unsigned int i;

#ifdef COMPILED_ROMS
	for (i = 0; i < ARRAY_SIZE(comp_roms); i++) {
		if (strcmp(comp_roms[i].name, filename) == 0) {
			*dest_ptr = comp_roms[i].data;
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
		debug_log("Failed to open %s\n", filename);
		fprintf(stderr, "Failed to open %s\n", filename);
		return -1; //exit(1);
	}
	if (fseek(f, 0, SEEK_END) < 0)
		perror("fseek SEEK_END");
	size = ftell(f);
	if (fseek(f, 0, SEEK_SET) < 0)
		perror("fseek SEEK_SET");

	if (base == 0 && *size_ptr != 0 && size != *size_ptr) {
		fprintf(stderr, "ROM %s size expected %d, not %d\n", filename, *size_ptr, size);
	}

	if (!dest || base + size > *size_ptr) {
		*size_ptr = (base + size + 0x1fff) & ~0x1fff; // round up to nearest 8k
		dest = my_realloc(dest, *size_ptr);
		*dest_ptr = dest;
	}
	dest += base/2; // base is bytes, dest is word ptr
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
		return -1; //exit(1);
	}
	return 0;
}

static int load_grom(char *filename, u8 **dest_ptr, unsigned int *size_ptr)
{
	unsigned int size = size_ptr ? *size_ptr : 0;

#ifdef COMPILED_ROMS
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(comp_roms); i++) {
		if (strcmp(comp_roms[i].name, filename) == 0) {
			*dest_ptr = comp_roms[i].data;
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
		debug_log("Failed to open %s\n", filename);
		fprintf(stderr, "Failed to open %s\n", filename);
		return -1; //exit(1);
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
		printf("%d\n", n);
	}
	fclose(f);
	return 0;
}


static void print_name_table(u8* reg, u8 *ram)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned char *line = ram + (reg[2]&0xf)*0x400;
	int y, x, w = (reg[1] & 0x10) ? 40 : 32;
	for (y = 0; y < 24; y++) {
		for (x = 0; x < w; x++) {
			unsigned char c = *line++;
			printf("%c%c",
				c >= ' ' && c < 127 ? c   : hex[c>>4],
				c >= ' ' && c < 127 ? ' ' : hex[c&15]);
		}
		printf("\n");
	}

}


#ifndef USE_SDL
void vdp_init(void)
{
}

void vdp_done(void)
{
}

int vdp_update(void)
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

void vdp_line(unsigned int y, u8* restrict reg, u8* restrict ram)
{
}

void vdp_text_pat(unused unsigned char *pat)
{
}

void vdp_text_window(const char *line, int w, int h, int x, int y, int highlight_line)
{
}

void vdp_set_fps(int mfps /* fps*1000 */)
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

static int breakpoint_address = -1;
static int breakpoint_bank = -1;


int breakpoint_read(u16 address) // called from brk_r() before read
{
	// TODO scan list of breakpoints to see if one is hit

	// don't break if single-stepping!
	if (address != breakpoint_address || debug_break == 2 || debug_en == 0)
		return 0;
	//printf("%s: address=%04X bp_bank=%d cart_bank=%d\n", __func__,
	//	address, breakpoint_bank, cart_bank);
	if (address >= 0x6000 && address < 0x8000) {
		if (breakpoint_bank != -1 && breakpoint_bank != cart_bank)
			return 0;
	}

	return 1;
}

int breakpoint_write(u16 address) // called from brk_w() after write
{
	// TODO scan list of breakpoints to see if one is hit

	return 0;
}

void toggle_breakpoint(u16 address, int bank)
{
	if (breakpoint_address != address) {
		breakpoint_address = address;
		breakpoint_bank = bank;
		clear_all_breakpoints();
		set_breakpoint(address, 2/*bytes*/);
	} else {
		breakpoint_address = -1;
		clear_all_breakpoints();
	}
}





// set cart name before doing a reset
void set_cart_name(char *name)
{
	free(cartridge_name);
	cartridge_name = name;
}

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

	debug_log("initial WP=%04X PC=%04X ST=%04X\n", get_wp(), get_pc(), get_st());

 	ga = 0xb5b5; // grom address
	grom_last = 0xaf;

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
		if (load_rom(cartridge_name, &cart_rom, &cart_rom_size, 0) == 0) {
			// loaded success, try d rom
			if (tolower(cartridge_name[len-5]) == 'c' && cart_rom_size == 8192) {
				char *name = malloc(len + 1);
				memcpy(name, cartridge_name, len + 1);
				// if '*c.bin' TODO check for '*d.bin'
				name[len-5]++; // C to D
				load_rom(name, &cart_rom, &cart_rom_size, 8192);
				free(name);
			}
		}

		if (cart_rom) {
			unsigned int banks = (cart_rom_size + 0x1fff) >> 13;

			cart_bank_mask = banks ? (1 << (32-__builtin_clz(banks))) - 1 : 0;
			if (0)printf("cart_bank_mask = 0x%x  (size=%d banks=%d) page_size=%d\n",
				cart_bank_mask, cart_rom_size, banks, 256/*1<<MAP_SHIFT*/);
			set_cart_bank(0);
		}

		// optionally load GROM
		{
			char *name = malloc(len + 2);

			// try replace 8/9/C.bin with G.bin
			memcpy(name, cartridge_name, len + 1);
			name[len-5] = isupper(name[len-5]) ? 'G' : 'g';

			printf("load grom %s\n", name);
		        if (load_grom(name, &cart_grom, &cart_grom_size) == -1) {
				// try insert G otherwise
				memcpy(name, cartridge_name, len + 1);
				memmove(name+len-3, name+len-4, 5); // move the .bin
				name[len-4] = isupper(name[len-5]) ? 'G' : 'g';

				printf("load grom %s\n", name);
				load_grom(name, &cart_grom, &cart_grom_size);
			}
			free(name);
			printf("grom=%p size=%u\n", cart_grom, cart_grom_size);
		}
		// optionally load listing
		{
			char *name = malloc(len + 1);
			memcpy(name, cartridge_name, len + 1);
			// replace .bin with .lst
			name[len-3] ^= 'b' ^ 'l';
			name[len-2] ^= 'i' ^ 's';
			name[len-1] ^= 'n' ^ 't';
			printf("load listing %s\n", name);
			load_listing(name, -1);
			free(name);
		}
		toggle_breakpoint(0x601e, -1);
	}
}


void redraw_vdp(void)
{
	int y;
	for (y = 0; y < 240; y++) {
		vdp_line(y, vdp.reg, vdp.ram);
	}
}


int vdp_update_or_menu(void)
{
	if (vdp_update() != 0)
		return -1; // quitting
	if (keyboard_down[0] & (1 << TI_MENU)) {
		memset(&keyboard_down, 0, sizeof(keyboard_down)); // clear keys
		mute(1);
		if (main_menu() == -1)
			return -1; // quitting
		if (!debug_break)
			mute(0);
	}

	return 0;
}


void update_debug_window(void)
{
	char reg[53*30] = { [0 ... 53*30-1]=32};
	u16 pc, wp;

	pc = get_pc();
	wp = get_wp();
	//memset(asm_text, 0, sizeof(asm_text));
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
		"  Y: %-3d        R 6: %04X\n"
		" BANK: %-4d     R 7: %04X\n"
		"                R 8: %04X\n"
		"                R 9: %04X\n"
		"                R10: %04X\n"
		"                R11: %04X\n"
		"                R12: %04X\n"
		"                R13: %04X\n"
		"                R14: %04X\n"
		"                R15: %04X\n"
		" KB: ROW: %d\n"
		" %02X %02X %02X %02X %02X %02X %02X %02X\n\n"
		"%s        (%d)\n                                          \n",
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
		vdp.y,	    safe_r(wp+12),
		cart_bank,  safe_r(wp+14),
			    safe_r(wp+16),
			    safe_r(wp+18),
			    safe_r(wp+20),
			    safe_r(wp+22),
			    safe_r(wp+24),
			    safe_r(wp+26),
			    safe_r(wp+28),
			    safe_r(wp+30),
		keyboard_row,
		keyboard[0], keyboard[1], keyboard[2], keyboard[3],
		keyboard[4], keyboard[5], keyboard[6], keyboard[7],
		asm_text, disasm_cyc);
		vdp_text_window(reg, 26,30, 0,240, -1);


		int i, n = 0;
		for (i = 0x8300; i < 0x8400; i+= 16) {
			n += sprintf(reg+n, "  %04X:  %04X %04X %04X %04X  %04X %04X %04X %04X\n",
				i, safe_r(i), safe_r(i+2), safe_r(i+4), safe_r(i+6), 
				safe_r(i+8), safe_r(i+10), safe_r(i+12), safe_r(i+14));
		}
		reg[n] = 0;
		vdp_text_window(reg, 53,30, 322,0, -1);
	}
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
		int len = slash - argv[0];
		argv0_dir_name = malloc(len + 1);
		memcpy(argv0_dir_name, argv[0], len);
		argv0_dir_name[len] = 0;
		//fprintf(stderr, "dir_name = %s\n", argv0_dir_name);
	}

	rom_size = 8192;
	load_rom("994arom.bin", &rom, &rom_size, 0);
	grom_size = 24576;
	load_grom("994agrom.bin", &grom, &grom_size);
	load_listing("994arom.lst", -1);
	if (!rom || !grom) {
		fprintf(stderr, "Failed to load ROM/GROM files: %s %s\n",
			rom ? "" : "994arom.bin",
			grom ? "" : "994agrom.bin");
		exit(0);
	}

	// Give GROM char patterns for debugger
	vdp_text_pat(grom + 0x06B4 - 32*7);

	if (argc > 1) {
		set_cart_name(
#ifdef _WIN32
				_strdup
#else
				strdup
#endif
				(argv[1])); // will get loaded on reset
		//load_rom(argv[1], &cart_rom, &cart_rom_size, 0);
	} else {
		//load_rom("../phantis/phantisc.bin", &cart_rom, &cart_rom_size, 0);
		//load_rom("cputestc.bin", &cart_rom, &cart_rom_size, 0);
		//load_rom("../wordit/wordit8.bin", &cart_rom, &cart_rom_size, 0);
#ifdef TEST
		//load_rom("cputestc.bin", &cart_rom, &cart_rom_size, 0);
		//load_rom("test/mbtest.bin", &cart_rom, &cart_rom_size, 0);
		load_rom("mbtest.bin", &cart_rom, &cart_rom_size, 0);
#endif
	}

	vdp_init();

	reset();
	printf("initial PC=%04X WP=%04X ST=%04X\n", get_pc(), get_wp(), get_st());

	//add_breakpoint(BREAKPOINT, 0x3aa, 0, 0, 0);
#ifndef TEST
	//debug_en = 1; debug_break = 1;
#endif

#ifndef TEST
	//load_listing("romsrc/ROM-4A.lst", 0);
	//load_listing("../bnp/bnp.lst", 0);
	//load_listing("fcmd/FCMD.listing", 0);
#endif

	int lines_per_frame = 262; // NTSC=262 PAL=313

	while (vdp_update_or_menu() == 0) {
		if (debug_en) {
			if (debug_window() == -1) break;
		}

		// render one frame
		do {
			// render one scanline
			if (vdp.y < 240) {
				vdp_line(vdp.y, vdp.reg, vdp.ram);
			} else if (vdp.y == 246) {
				vdp.reg[8] |= 0x80;  // set F in VDP status
				if (vdp.reg[1] & 0x20) // check IE
					interrupt(1);  // VDP interrupt
			}
			if (++vdp.y == lines_per_frame) {
				vdp.y = 0;
			}

			total_cycles += CYCLES_PER_LINE;
			add_cyc(-CYCLES_PER_LINE);
			if (debug_break == 2) {
				single_step();
				debug_break = 1;
				break;
			}
			emu(); // emulate until cycle counter goes positive

		} while (vdp.y != 0 && debug_break == 0);
	}

	//debug_log("%d\n", total_cycles);
	vdp_done();
#ifdef TEST
	print_name_table(vdp.reg, vdp.ram);
#endif

	if (log) fclose(log);
	if (disasmf && disasmf != log) fclose(disasmf);
}
