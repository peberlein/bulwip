/*
 *  cpu.c - TMS 9900 CPU emulation
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
#include <stdlib.h>

#include "cpu.h"



// program counter, workspace pointer, status flags
static u16 gPC, gWP; // st is below with status functions
static int cyc = 0; // cycle counter (cpu runs until > 0)
static int interrupt_level = 0; // interrupt level + 1  (call interrupt_check() after modifying)
static int breakpoint_saved_cyc = 0; // when a breakpoint is hit, this saves cyc before it gets zeroed
int trace = 0;

u16 get_pc(void) { return gPC; }
u16 get_wp(void) { return gWP; }
u16 get_st(void); // defined below with status functions
int add_cyc(int add) {
	//if (trace && add) printf("+%d\n", add);
	cyc += add;
	return cyc;
}

// needed by undo stack
void set_pc(u16 pc) { gPC = pc; }
void set_wp(u16 wp) { gWP = wp; }
void set_st(u16 st); // defined below with status functions
void set_cyc(s16 c) { cyc = c; }


void single_step(void)
{
	int old_pc = gPC;
	int saved_cyc = cyc;

	cyc = 0;
	emu(); // this will return after 1 instruction when cyc=0
	if (trace) {
		disasm(old_pc, cyc);
		printf("%s", asm_text);
	}
#ifdef ENABLE_UNDO
	// Since emu was called with cyc=0, the undo stack will also
	// store UNDO_CYC=0, which will mess up the cycle counts in
	// the disassembly. Fix it with the original cycle counter.
	undo_fix_cyc((u16)saved_cyc);
#endif
	cyc += saved_cyc;
	//printf("%s: save=%d cyc=%d\n", __func__, saved_cyc, cyc);
}



// forward
static u16 brk_r(u16 address);
static void brk_w(u16 address, u16 value);

/******************************************
 * Memory page mapping functions          *
 ******************************************/

// Function pointers for accessing memory in PAGE-sized chunks.
// Functions suitable for each memory mapped device may be 
// efficiently selected in this way.
// A breakpoint will replace the accessor function for only
// the page the breakpoint is in, leaving other pages unaffected.
// Note: The "safe read" function variant is required to not have 
// side-effects, i.e. no auto-incrementing read address or clearing flags,
// making it suitable for examining memory in debugger for instance.

// Use 12 for 4K pages, or 10 for 1K pages (0x400) or 8 for 256B pages
//#define MAP_SHIFT 12
#define MAP_SHIFT 8
#define PAGE_SIZE (1 << MAP_SHIFT)
#define PAGE_MASK (PAGE_SIZE-1)
#define PAGES_IN_64K (1<<(16-MAP_SHIFT))

#define ALTERNATE_MAPPING
#ifdef ALTERNATE_MAPPING
u16 (*map_read_func[PAGES_IN_64K])(u16) = {NULL};
void (*map_write_func[PAGES_IN_64K])(u16, u16) = {NULL};
u16 (*map_read_orig_func[PAGES_IN_64K])(u16) = {NULL};
void (*map_write_orig_func[PAGES_IN_64K])(u16, u16) = {NULL};
u16 (*map_safe_read_func[PAGES_IN_64K])(u16) = {NULL};
u16 *map_mem_addr[PAGES_IN_64K] = {NULL};

#define map_read(x) map_read_func[x]
#define map_write(x) map_write_func[x]
#define map_read_orig(x) map_read_orig_func[x]
#define map_write_orig(x) map_write_orig_func[x]
#define map_safe_read(x) map_safe_read_func[x]
#define map_mem(x) map_mem_addr[x]

#else
static struct {
	u16 (*read)(u16);         // read function
	u16 (*safe_read)(u16);    // read function without side-effects (but may increment cyc)
	void (*write)(u16, u16);  // write function
	u16 *mem;                 // memory reference
} map[PAGES_IN_64K]; // N (1<<MAP_SHIFT) banks

#define map_read(x) map[x].read
#define map_safe_read(x) map[x].safe_read
#define map_write(x) map[x].write
#define map_mem(x) map[x].mem
#endif

void change_mapping(int base, int size, u16 *mem)
{
	int i;
	for (i = 0; i < size; i += PAGE_SIZE) {
		map_mem(base >> MAP_SHIFT) = mem + (i>>1);
		base += PAGE_SIZE;
	}
}

void set_mapping_safe(int base, int size,
	u16 (*read)(u16),
	u16 (*safe_read)(u16),
	void (*write)(u16, u16),
	u16 *mem)
{
	int i, end;
	end = (base + size) >> MAP_SHIFT;
	base >>= MAP_SHIFT;
	for (i = base; i < end; i++) {
		if (map_read(i) != brk_r)
			map_read(i) = read;
		map_read_orig(i) = read;
		map_safe_read(i) = safe_read;
		if (map_write(i) != brk_w)
			map_write(i) = write;
		map_write_orig(i) = write;
		map_mem(i) = mem ? mem + ((i-base) << (MAP_SHIFT-1)) : NULL;
		//printf("map page=%x address=%p\n", base+i, map[base+i].mem);
	}
}

void set_mapping(int base, int size,
	u16 (*read)(u16),
	void (*write)(u16, u16),
	u16 *mem)
{
	set_mapping_safe(base, size, read, read, write, mem);
}



/******************************************
 * Memory accessor functions              *
 ******************************************/

#define always_inline inline __attribute((always_inline))

// These may be overridden by debugger for breakpoints/watchpoints
//static u16 (*iaq_func)(u16); // instruction acquisition

static always_inline u16 mem_r(u16 address)
{
	u16 value = map_read(address >> MAP_SHIFT)(address);
	return value;
}

u16 safe_r(u16 address)
{
	int saved_cyc = cyc;
	u16 value = map_safe_read(address >> MAP_SHIFT)(address);
	cyc = saved_cyc;
	return value;
}

static always_inline void mem_w(u16 address, u16 value)
{
	map_write(address >> MAP_SHIFT)(address, value);
}

static always_inline u16 reg_r(u16 wp, u8 reg)
{
	return mem_r(wp + 2 * reg);
}

static always_inline void reg_w(u16 wp, u8 reg, u16 value)
{
	mem_w(wp + 2 * reg, value);
}

static void dump_regs(void)
{
	int i;
	for (i = 0; i < 16; i++) {
		debug_log("R%-2d:%04X ", i, safe_r(gWP + 2 * i));
		if (i == 7) {
			debug_log("  PC=%04X  WP=%04x\n", gPC, gWP);
		} else if (i == 15) {
			debug_log("  ST=%04x\n", get_st());
		}
	}
}

u16 map_r(u16 address)
{
	u8  page = address >> MAP_SHIFT;
	u16 offset = address & PAGE_MASK;
	//printf("%04x page=%x offset=%x value=%04x\n", address, page, offset,
	//		map[page].mem[offset>>1]);

	if (map_mem(page) == NULL) {
		debug_log("no memory mapped at %04X (read)\n", address);
		return 0;
	}
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
	return map_mem(page)[offset >> 1];
}

void map_w(u16 address, u16 value)
{
	u8  page = address >> MAP_SHIFT;
	u16 offset = address & PAGE_MASK;
	if (map_mem(page) == NULL) {
		debug_log("no memory mapped at %04X (write, %04X)\n", address, value);
		return;
	}
	add_cyc(6); // 2 cycles for memory access + 4 for multiplexer
#ifdef ENABLE_UNDO
	undo_push(UNDO_EXPRAM,
		((address & (address & 0x8000 ? 0x7ffe : 0x1ffe)) << 15) |
		map_mem(page)[offset >> 1]);
#endif
	map_mem(page)[offset >> 1] = value;
}

#ifdef ENABLE_DEBUGGER

// Breakpoint read
static u16 brk_r(u16 address)
{
	//printf("%s: %04X\n", __func__, address);
	if (breakpoint_read(address)) {
		breakpoint_saved_cyc = cyc;
		if (address == gPC) {
			debug_break = DEBUG_STOP;
			return C99_BRK; // instruction decoder will handle this
		} else {
			cyc = 0; // memory read trigger break: return after current instruction
		}
	}
	return map_read_orig(address >> MAP_SHIFT)(address);
}

// Breakpoint write
static void brk_w(u16 address, u16 value)
{
	map_write_orig(address >> MAP_SHIFT)(address, value);
	if (breakpoint_write(address)) {
		breakpoint_saved_cyc = cyc;
		cyc = 0; // memory write trigger break: return after current instruction
		debug_break = DEBUG_STOP;
	}
}

// Reset any breakpoint mappings to the original mappings
void cpu_reset_breakpoints(void)
{
	int i;
	for (i = 0; i < PAGES_IN_64K; i++) {
		map_read(i) = map_read_orig(i);
		map_write(i) = map_write_orig(i);
	}
}

void cpu_set_breakpoint(u16 base, u16 size)
{
	int i, end;
	end = (base + size) >> MAP_SHIFT;
	base >>= MAP_SHIFT;
	for (i = base; i <= end; i++) {
		map_read(i) = brk_r;
		map_write(i) = brk_w;
	}
}

#endif // ENABLE_DEBUGGER



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


u16 get_st(void) { return ((u16)st_flg << 8) | st_int; }
void set_st(u16 new_st) { st_flg = new_st >> 8; st_int = new_st & ST_IM; }
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


static always_inline int shift_count(u16 op, u16 wp)
{
	int count = (op >> 4) & 15 ?: reg_r(wp, 0) & 15 ?: 16;
	cyc += 2 * count;
	return count;
}



/*************************************************
 * Ts source and Td destination operand decoding *
 *************************************************/

// bytes=2 source returned as word value (indirects are followed)
// bytes=1 source returned as byte value (in high byte, low byte is zero)
static always_inline u16 Ts(u16 op, u16 *pc, u16 wp, u8 bytes)
{
	u16 val, addr = 0;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx
		return bytes == 2 ? reg_r(wp, reg) : reg_r(wp, reg) & 0xff00;
	case 1: // *Rx
		addr = reg_r(wp, reg);
		val = mem_r(addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		if (reg) addr = reg_r(wp, reg); else cyc += 2;
		addr += mem_r(*pc);
		*pc += 2;
		val = mem_r(addr);
		cyc += 4;
		break;
	case 3: // *Rx+
		addr = reg_r(wp, reg);
		reg_w(wp, reg, addr + bytes); // NOTE: increment before fetch!
		val = mem_r(addr);   // read value, then update
		cyc += bytes * 2; // word=4 byte=2
		break;
	}
	return bytes == 2 ? val :
		(addr & 1) ? val << 8 : val & 0xff00;
}

struct val_addr {
	u16 val, addr;
};

static always_inline struct val_addr Td(u16 op, u16 *pc, u16 wp, u16 bytes)
{
	struct val_addr va;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx  2c
		va.addr = wp + 2 * reg;
		va.val = reg_r(wp, reg);
		break;
	case 1: // *Rx  6c
		va.addr = reg_r(wp, reg);
		va.val = mem_r(va.addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0  10c
		va.addr = 0;
		if (reg) va.addr = reg_r(wp, reg); else cyc += 2;
		va.addr += mem_r(*pc);
		*pc += 2;
		va.val = mem_r(va.addr);
		cyc += 4;
		break;
	case 3: // *Rx+   10c
		va.addr = reg_r(wp, reg);
		reg_w(wp, reg, va.addr + bytes); // NOTE: increment before fetch!
		va.val = mem_r(va.addr);
		cyc += bytes * 2; // word=4 byte=2
		break;
	}
	return va;
}


static always_inline void byte_op(u16 *op, u16 *pc, u16 wp, u16 *ts, struct val_addr *td)
{
	*ts = Ts(*op, pc, wp, 1);
	*op >>= 6;
	*td = Td(*op, pc, wp, 1);
}

static always_inline void word_op(u16 *op, u16 *pc, u16 wp, u16 *ts, struct val_addr *td)
{
	*ts = Ts(*op, pc, wp, 2);
	*op >>= 6;
	*td = Td(*op, pc, wp, 2);
}


/****************************************
 * Reset and Interrupts                 *
 ****************************************/

void cpu_reset(void)
{
	int saved_cyc = cyc;

	// on reset
	gWP = mem_r(0);
	gPC = mem_r(2);
	set_st(0xc3f0); //0x0060;

	cyc = saved_cyc;
}

void interrupt(int level)
{
	if (level == -1 || level > (get_st() & 15)) {
		//debug_log("interrupt %d declined ST=%x PC=%04X\n", level, st & 15, pc);
		// save it later to be checked by change to LIMI or RTWP
		interrupt_level = level + 1;
	} else {
		u16 wp = mem_r(level * 4);
		//debug_log("OLD pc=%04X wp=%04X st=%04X\n", pc, wp, st);
		mem_w(wp + 2*13, gWP);
		mem_w(wp + 2*14, gPC);
		mem_w(wp + 2*15, get_st());
		gPC = mem_r(level * 4 + 2);
		gWP = wp;
		st_int = (st_int & 0xf0) | (level ? level-1 : 0);
		//debug_log("interrupt %d pc=%04X wp=%04X st=%04X\n", level, pc, wp, st);
	}
}

// call this after something changes the interrupt level in status
static void check_interrupt_level()
{
	if (interrupt_level)
		interrupt(interrupt_level - 1);
}



// Instruction opcode decoding table - indexed by count of MSB zeroes

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


// idea for faster indirect memory read/write
// keep a cache of read/write functions for all 16 registers
// cache entries default to a function that updates the cache
// 16 default functions, one for each register
// changing workspace pointer resets the function cache
// writing to a register resets the cache entry
// writing to memory WP to WP+31 resets the cache entry


// This maps instruction opcodes from 16 bits to 7 bits, making switch lookup table more efficient
#define DECODE(x) ((((x) >> (24-__builtin_clz(x))) & 0x78) | (22-__builtin_clz(x)))

// instruction opcode decoding using count-leading-zeroes (clz)

void emu(void)
{
	u16 op, pc = gPC, wp = gWP;
	u16 ts;
	struct val_addr td;
#ifdef LOG_DISASM
	int start_cyc;
#endif

	goto start_decoding;
decode_op:
#ifdef LOG_DISASM
	disasm(gPC, cyc - start_cyc);
	debug_log("%s", asm_text);
#endif
	// when cycle counter rolls positive, go out and render a scanline
	if (cyc > 0)
		goto done;
	goto start_decoding;

decode_op_now: // this is used by instructions which cannot be interrupted (XOP, BLWP)
#ifdef LOG_DISASM
	disasm(gPC, cyc - start_cyc);
	debug_log("%s", asm_text);
#endif
	gPC = pc;  // breakpoints will need to know the current PC
	goto start_decoding_skip_undo;
start_decoding:
	gPC = pc;  // breakpoints will need to know the current PC
	if (0/*trace*/) {
		gWP = wp;
		disasm(pc, cyc);
		//debug_log("%s", asm_text);
	}
#ifdef LOG_DISASM
	start_cyc = cyc;
#endif
	undo_push(UNDO_PC, pc);
	undo_push(UNDO_CYC, (u16)cyc);
	undo_push(UNDO_ST, get_st());
start_decoding_skip_undo:

	op = mem_r(pc);
	// if this mem read triggers a breakpoint, it will save the cycles
	// before reading the memory and will return C99_BRK
	pc += 2;
execute_op:
	cyc += 6; // base cycles

	switch (DECODE(op)) {
	LI:   case DECODE(0x0200): reg_w(wp, op&15, status_zero(mem_r(pc))); pc += 2; goto decode_op;
	AI:   case DECODE(0x0220): reg_w(wp, op&15, add(reg_r(wp, op&15), mem_r(pc))); pc += 2; goto decode_op;
	ANDI: case DECODE(0x0240): reg_w(wp, op&15, status_zero(reg_r(wp, op&15) & mem_r(pc))); pc += 2; goto decode_op;
	ORI:  case DECODE(0x0260): reg_w(wp, op&15, status_zero(reg_r(wp, op&15) | mem_r(pc))); pc += 2; goto decode_op;
	CI:   case DECODE(0x0280): status_arith(reg_r(wp, op&15), mem_r(pc)); cyc += 2; pc += 2; goto decode_op;
	STWP: case DECODE(0x02A0): reg_w(wp, op&15, wp); goto decode_op;
	STST: case DECODE(0x02C0): reg_w(wp, op&15, get_st()); goto decode_op;
	LWPI: case DECODE(0x02E0): cyc -= 2; undo_push(UNDO_WP, wp); wp = mem_r(pc); cyc += 2; pc += 2; goto decode_op;
	LIMI: case DECODE(0x0300): cyc -= 2; undo_push(UNDO_WP, wp); set_IM(mem_r(pc) & 15); pc += 2; gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; goto decode_op;

	IDLE: case DECODE(0x0340): debug_log("IDLE not implemented\n");/* TODO */ goto decode_op;
	RSET: case DECODE(0x0360): debug_log("RSET not implemented\n"); /* TODO */ goto decode_op;
	RTWP: case DECODE(0x0380): undo_push(UNDO_WP, wp); set_st(reg_r(wp, 15)); pc = reg_r(wp, 14); wp = reg_r(wp, 13); gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; goto decode_op;
	CKON: case DECODE(0x03A0): debug_log("CKON not implemented\n");/* TODO */ goto decode_op;
	CKOF: case DECODE(0x03C0): debug_log("CKOF not implemented\n");/* TODO */ goto decode_op;
	LREX: case DECODE(0x03E0): debug_log("LREX not implemented\n");/* TODO */ goto decode_op;
	BLWP: case DECODE(0x0400):
		cyc += 8;
		undo_push(UNDO_WP, wp);
		td = Td(op, &pc, wp, 2);
		//debug_log("OLD pc=%04X wp=%04X st=%04X  NEW pc=%04X wp=%04X\n", pc, wp, st, safe_r(va.addr+2), va.val);
		mem_w(td.val + 2*13, wp);
		mem_w(td.val + 2*14, pc);
		mem_w(td.val + 2*15, get_st());
		pc = mem_r(td.addr + 2);
		wp = td.val;
		goto decode_op_now; // next instruction cannot be interrupted
	B:    case DECODE(0x0440): cyc -= 2; td = Td(op, &pc, wp, 2); pc = td.addr; goto decode_op;
	X:    case DECODE(0x0480): cyc -= 2; td = Td(op, &pc, wp, 2); op = td.val; /*printf("X op=%x pc=%x\n", op, pc);*/ goto execute_op;
	CLR:  case DECODE(0x04C0): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = 0; mem_w(td.addr, td.val); goto decode_op;
	NEG:  case DECODE(0x0500): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = sub(0, td.val); mem_w(td.addr, td.val); goto decode_op;
	INV:  case DECODE(0x0540): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = status_zero(~td.val); mem_w(td.addr, td.val); goto decode_op;
	INC:  case DECODE(0x0580): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = add(td.val,1); mem_w(td.addr, td.val); goto decode_op;
	INCT: case DECODE(0x05C0): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = add(td.val,2); mem_w(td.addr, td.val); goto decode_op;
	DEC:  case DECODE(0x0600): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = sub(td.val,1); mem_w(td.addr, td.val); goto decode_op;
	DECT: case DECODE(0x0640): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = sub(td.val,2); mem_w(td.addr, td.val); goto decode_op;
	BL:   case DECODE(0x0680): cyc -= 2; td = Td(op, &pc, wp, 2); reg_w(wp, 11, pc); pc = td.addr; goto decode_op;
	SWPB: case DECODE(0x06C0): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = swpb(td.val); mem_w(td.addr, td.val); goto decode_op;
	SETO: case DECODE(0x0700): cyc -= 2; td = Td(op, &pc, wp, 2); td.val = 0xffff; mem_w(td.addr, td.val); goto decode_op;
	ABS:  case DECODE(0x0740):
		td = Td(op, &pc, wp, 2);
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
		mem_w(td.addr, td.val);
		goto decode_op;
	SRA: case DECODE(0x0800): case DECODE(0x0880): {
		u16 val = reg_r(wp, op & 15);
		u8 count = shift_count(op, wp);
		if (val & (1 << (count-1))) set_C(); else clr_C();
		reg_w(wp, op & 15, status_zero(((s16)val) >> count));
		goto decode_op; }
	SRL: case DECODE(0x0900): case DECODE(0x0980): {
		u16 val = reg_r(wp, op & 15);
		u8 count = shift_count(op, wp);
		if (val & (1 << (count-1))) set_C(); else clr_C();
		reg_w(wp, op & 15, status_zero(val >> count));
		goto decode_op; }
	SLA: case DECODE(0x0A00): case DECODE(0x0A80): {
		u16 val = reg_r(wp, op & 15);
		u8 count = shift_count(op, wp);
		if (val & (0x8000 >> (count-1))) set_C(); else clr_C();
		reg_w(wp, op & 15, status_zero(val << count));
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
		u16 val = reg_r(wp, op & 15);
		u8 count = shift_count(op, wp);
		if (val & (1 << (count-1))) set_C(); else clr_C();
		reg_w(wp, op & 15, status_zero((val << (16-count))|(val >> count)));
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
        SBO: case DECODE(0x1D00): cru_w((op & 0xff) + ((reg_r(wp, 12) & 0x1ffe) >> 1), 1); goto decode_op;
        SBZ: case DECODE(0x1E00): cru_w((op & 0xff) + ((reg_r(wp, 12) & 0x1ffe) >> 1), 0); goto decode_op;
        TB:  case DECODE(0x1F00): status_equal(cru_r((op & 0xff) + ((reg_r(wp, 12) & 0x1ffe) >> 1)), 1); goto decode_op;

	COC: case DECODE(0x2000): case DECODE(0x2200): {
		u16 ts = Ts(op, &pc, wp, 2);
		u16 td = reg_r(wp, (op >> 6) & 15);
		if ((ts & td) == ts) set_EQ(); else clr_EQ();
		cyc += 2;
		goto decode_op; }
	CZC: case DECODE(0x2400): case DECODE(0x2600): {
		u16 ts = Ts(op, &pc, wp, 2);
		u16 td = reg_r(wp, (op >> 6) & 15);
		if (!(ts & td)) set_EQ(); else clr_EQ();
		cyc += 2;
		goto decode_op; }
	XOR: case DECODE(0x2800): case DECODE(0x2A00): {
		u8 reg = (op >> 6) & 15;
		u16 ts = Ts(op, &pc, wp, 2);
		u16 td = reg_r(wp, reg);
		reg_w(wp, reg, status_zero(ts ^ td));
		goto decode_op; }
	XOP: case DECODE(0x2C00): case DECODE(0x2E00): {
		struct val_addr td = Td(op, &pc, wp, 2); // source
		u8 reg = (op >> 6) & 15; // XOP number usually 1 or 2
		u16 ts = mem_r(0x0040 + (reg << 2)); // new WP

		undo_push(UNDO_WP, wp);
		mem_w(ts + 2*11, td.addr); // gAS copied to R11
		mem_w(ts + 2*13, wp); // WP to R13
		mem_w(ts + 2*14, pc); // PC to R14
		mem_w(ts + 2*15, get_st()); // ST to R15
		pc = mem_r(0x0042 + (reg << 2));
		wp = ts;
		set_X();
		goto decode_op_now; } // next instruction cannot be interrupted

	LDCR: case DECODE(0x3000): case DECODE(0x3200): {
		u8 idx, c = ((op >> 6) & 15) ?: 16;
		u16 ts, reg = (reg_r(wp, 12) & 0x1ffe) >> 1;
		if (c <= 8) {
			ts = Ts(op, &pc, wp, 1) >> 8;
			status_parity(ts);
		} else {
			ts = Ts(op, &pc, wp, 2);
		}
		for (idx = 0; idx < c; idx++)
			cru_w(reg + idx, (ts >> idx) & 1);
		status_zero(ts);
		goto decode_op; }
	STCR: case DECODE(0x3400): case DECODE(0x3600): {
		u8 idx, c = ((op >> 6) & 15) ?: 16;
		u16 reg = (reg_r(wp, 12) & 0x1ffe) >> 1;
		if (c <= 8) {
			td = Td(op, &pc, wp, 1);
			td.val &= (td.addr & 1) ? 0xff00 : 0x00ff;
			u16 base = (td.addr & 1) ? 1 : 0x100;
			for (idx = 0; idx < c; idx++) {
				if (cru_r(reg + idx))
					td.val |= (base << idx);
			}
			mem_w(td.addr, td.val);
			status_parity(status_zero(td.val & 0xff00));
		} else {
			td = Td(op, &pc, wp, 2);
			td.val = 0;
			for (idx = 0; idx < c; idx++) {
				if (cru_r(reg + idx))
					td.val |= (1 << idx);
			}
			mem_w(td.addr, td.val);
			status_zero(td.val);
		}
		// TODO status bits
		goto decode_op; }
	MPY: case DECODE(0x3800): case DECODE(0x3A00): {
		u32 val = Ts(op, &pc, wp, 2);
		u8 reg = (op >> 6) & 15;
		//debug_log("MPY %04X x %04X = %08X\n", 
		//	val, reg_r(wp, reg), val * reg_r(wp, reg));
		val *= reg_r(wp, reg);
		reg_w(wp, reg, val >> 16);
		reg_w(wp, reg+1, val & 0xffff);
		goto decode_op; }
	DIV: case DECODE(0x3C00): case DECODE(0x3E00): {
		u32 val;
		u16 ts = Ts(op, &pc, wp, 2);
		u8 reg = (op >> 6) & 15;
		val = reg_r(wp, reg);
		if (ts <= val) {
			set_OV();
		} else {
			clr_OV();
			val = (val << 16) | reg_r(wp, reg+1);
			//debug_log("DIV %08X by %04X\n", val, ts);
			reg_w(wp, reg, val / ts);
			reg_w(wp, reg+1, val % ts);
		}
		goto decode_op; }

	SZC: case DECODE(0x4000): case DECODE(0x4400): case DECODE(0x4800): case DECODE(0x4C00):
		word_op(&op, &pc, wp, &ts, &td);
		td.val = status_zero(td.val & ~ts); mem_w(td.addr, td.val); goto decode_op;
	SZCB: case DECODE(0x5000): case DECODE(0x5400): case DECODE(0x5800): case DECODE(0x5C00):
		byte_op(&op, &pc, wp, &ts, &td);
		if (td.addr & 1) {
			td.val &= ~(ts >> 8);
			status_parity(status_zero(td.val << 8));
		} else {
			td.val &= ~ts;
			status_parity(status_zero(td.val & 0xff00));
		}
		mem_w(td.addr, td.val);
		goto decode_op;
	S: case DECODE(0x6000): case DECODE(0x6400): case DECODE(0x6800): case DECODE(0x6C00):
		word_op(&op, &pc, wp, &ts, &td);
		td.val = sub(td.val, ts); mem_w(td.addr, td.val); goto decode_op;
	SB: case DECODE(0x7000): case DECODE(0x7400): case DECODE(0x7800): case DECODE(0x7C00):
		byte_op(&op, &pc, wp, &ts, &td);
		if (td.addr & 1) {
			td.val = (td.val & 0xff00) | status_parity(sub(td.val << 8, ts) >> 8);
		} else {
			td.val = (td.val & 0x00ff) | status_parity(sub(td.val & 0xff00, ts));
		}
		mem_w(td.addr, td.val);
		goto decode_op;
	C: case DECODE(0x8000): case DECODE(0x8800):
		word_op(&op, &pc, wp, &ts, &td);
		status_arith(ts, td.val); cyc+=2; goto decode_op;
	CB: case DECODE(0x9000): case DECODE(0x9800):
		byte_op(&op, &pc, wp, &ts, &td);
		if (td.addr & 1) {
			status_arith(status_parity(ts), td.val << 8);
		} else {
			status_arith(status_parity(ts), td.val & 0xff00);
		}
		cyc += 2;
		goto decode_op;
	A: case DECODE(0xA000): case DECODE(0xA800):
		word_op(&op, &pc, wp, &ts, &td);
		td.val = add(td.val, ts); mem_w(td.addr, td.val); goto decode_op;
	AB: case DECODE(0xB000): case DECODE(0xB800):
		byte_op(&op, &pc, wp, &ts, &td);
		if (td.addr & 1) {
			td.val = (td.val & 0xff00) | status_parity(add(td.val << 8, ts) >> 8);
		} else {
			td.val = (td.val & 0x00ff) | status_parity(add(td.val & 0xff00, ts));
		}
		mem_w(td.addr, td.val);
		goto decode_op;
	MOV: case DECODE(0xC000): case DECODE(0xC800):
		word_op(&op, &pc, wp, &ts, &td);
		td.val = status_zero(ts); mem_w(td.addr, td.val); goto decode_op;
	MOVB: case DECODE(0xD000): case DECODE(0xD800):
		byte_op(&op, &pc, wp, &ts, &td);
		if (td.addr & 1) {
			td.val = (td.val & 0xff00) | status_parity(status_zero(ts) >> 8);
		} else {
			td.val = (td.val & 0x00ff) | status_parity(status_zero(ts));
		}
		mem_w(td.addr, td.val);
		goto decode_op;
	SOC: case DECODE(0xE000): case DECODE(0xE800):
		word_op(&op, &pc, wp, &ts, &td);
		td.val = status_zero(td.val | ts); mem_w(td.addr, td.val); goto decode_op;
	SOCB: case DECODE(0xF000): case DECODE(0xF800):
		byte_op(&op, &pc, wp, &ts, &td);
		if (td.addr & 1) {
			td.val |= (ts >> 8);
			status_parity(status_zero(td.val << 8));
		} else {
			td.val |= ts;
			status_parity(status_zero(td.val & 0xff00));
		}
		mem_w(td.addr, td.val);
		goto decode_op;

	default:
		{
			static int last_pc = -1;
			if (last_pc != pc-2)
				printf("illegal opcode at %04x: %04x  clz=%d\n", pc, op, op ? (int)__builtin_clz(op) : 0);
			last_pc = pc;
		}
		UNHANDLED:
		if (op == C99_BRK) {
#ifdef ENABLE_DEBUGGER
			if (debug_break == DEBUG_STOP) {
				pc -= 2; // set PC to before this instruction was executed
				goto done;
			}
#endif
		}
		unhandled(pc, op);
		goto decode_op;
	}
done:
	gPC = pc;
	gWP = wp;
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


static char reg_text[64] = {};
char asm_text[256] = {};


static u16 disasm_Ts(u16 pc, u16 op, int opsize)
{
	u16 i = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		sprintf(asm_text+strlen(asm_text), "R%d", i);
		sprintf(reg_text+strlen(reg_text), " R%d=%04X", i, safe_r(gWP+i*2));
		break;
	case 1: // *Rx
		sprintf(asm_text+strlen(asm_text), "*R%d", i);
		sprintf(reg_text+strlen(reg_text), " *(R%d=%04X)=%04X ", i, safe_r(gWP+i*2), safe_r(safe_r(gWP+i*2)));
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		pc += 2;
		sprintf(asm_text+strlen(asm_text), "@>%04X", safe_r(pc));
		if (i) {
			sprintf(asm_text+strlen(asm_text), "(R%d)", i);
			sprintf(reg_text+strlen(reg_text), " @>%04X(R%d=%X)=%04X", safe_r(pc), i, safe_r(gWP+i*2), safe_r(safe_r(pc)+safe_r(gWP+i*2)));
		} else {
			sprintf(reg_text+strlen(reg_text), " @>%04X=%04X", safe_r(pc), safe_r(safe_r(pc)));
		}
		break;
	case 3: // *Rx+
		sprintf(asm_text+strlen(asm_text), "*R%d+", i);
		sprintf(reg_text+strlen(reg_text), " *(R%d=%04X)+=%04X", i,
			safe_r(gWP+(op&15)*2) /*-opsize*/,    // subtracting opsize will undo the increment
			safe_r(safe_r(gWP+(op&15)*2)/*-opsize*/)); // but only if disassembling after executing
		break;
	}
	return pc;
}

static u16 disasm_Bs(u16 pc, u16 op, int opsize)
{
	u16 i = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		sprintf(asm_text+strlen(asm_text), "R%d", i);
		sprintf(reg_text+strlen(reg_text), " R%d=%04X", i, safe_r(gWP+i*2));
		break;
	case 1: // *Rx
		sprintf(asm_text+strlen(asm_text), "*R%d", i);
		sprintf(reg_text+strlen(reg_text), " R%d=%04X", i, safe_r(gWP+i*2));
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		pc += 2;
		sprintf(asm_text+strlen(asm_text), "@>%04X", safe_r(pc));
		if (i) {
			sprintf(asm_text+strlen(asm_text), "(R%d)", i);
			sprintf(reg_text+strlen(reg_text), " @>%04X(R%d=%X)=%04X", safe_r(pc), i, safe_r(gWP+i*2), safe_r(safe_r(pc)+safe_r(gWP+i*2)));
		} else {
			sprintf(reg_text+strlen(reg_text), " @>%04X=%04X", safe_r(pc), safe_r(safe_r(pc)));
		}
		break;
	case 3: // *Rx+
		sprintf(asm_text+strlen(asm_text), "*R%d+", i);
		sprintf(reg_text+strlen(reg_text), " *R%d=%04X", i, safe_r(safe_r(gWP+(op&15)*2)-opsize));
		break;
	}
	return pc;
}


// returns number of bytes in the disassembled instruction (2, 4, or 6)
int disasm(u16 pc, int cycles)
{
	static const int jt1[]={L8(C,CB,A,AB,MOV,MOVB,SOC,SOCB)};
	static const int jt2[]={L4(SZC,SZCB,S,SB)};
	static const int jt3[]={L8(COC,CZC,XOR,XOP,LDCR,STCR,MPY,DIV)};
	static const int jt4[]={L8(JMP,JLT,JLE,JEQ,JHE,JGT,JNE,JNC),L8(JOC,JNO,JL,JH,JOP,SBO,SBZ,TB)};
	static const int jt5[]={L4(SRA,SRL,SLA,SRC)};
	static const int jt6[]={L8(BLWP,B,X,CLR,NEG,INV,INC,INCT),L8(DEC,DECT,BL,SWPB,SETO,ABS,BAD,BAD)};
	static const int jt7[]={L8(LI,AI,ANDI,ORI,CI,STWP,STST,LWPI),L8(LIMI,BAD,IDLE,RSET,RTWP,CKON,CKOF,LREX)};
	static const int *jt[] = {jt1, jt2, jt3, jt4, jt5, jt6, jt7};
	int save_cyc = cyc;
	u16 wp = gWP;
	u16 op = safe_r(pc), ts, td;
	u8 idx = __builtin_clz(op)-16;
	u16 start_pc = pc;

	asm_text[0] = 0; // clear the disassembly string
	reg_text[0] = 0; // clear register value string
	// note: for breakpoints to work, PC must be at column 5 or 6
	if (pc >= 0x6000 && pc < 0x8000)
		sprintf(asm_text+strlen(asm_text), " %-3d %04X  %04X  ",
		 	get_cart_bank(), pc, op);
	else
		sprintf(asm_text+strlen(asm_text), "     %04X  %04X  ", pc, op);
	if (idx >= ARRAY_SIZE(jt)) {
		BAD:
		sprintf(asm_text+strlen(asm_text), "DATA >%04X", op);
		goto done;
	}
	u8 subidx = (op >> decode[idx].shift) & decode[idx].mask;

	if (!names[idx][subidx])
		goto BAD;
	sprintf(asm_text+strlen(asm_text), "%-5s", names[idx][subidx]);
	goto *(&&C + jt[idx][subidx]);

C: A: MOV: SOC: SZC: S:
	pc = disasm_Ts(pc, op, 2);
	sprintf(asm_text+strlen(asm_text), ",");
	pc = disasm_Ts(pc, op >> 6, 2);
	goto done;

CB: AB: MOVB: SOCB: SZCB: SB:
	pc = disasm_Ts(pc, op, 1);
	sprintf(asm_text+strlen(asm_text), ",");
	pc = disasm_Ts(pc, op >> 6, 1);
	goto done;
COC: CZC: XOR: MPY: DIV:
	pc = disasm_Ts(pc, op, 2);
	sprintf(asm_text+strlen(asm_text), ",R%d", (op >> 6) & 15);
	goto done;
XOP:
	pc = disasm_Ts(pc, op, 2);
	sprintf(asm_text+strlen(asm_text), ",%d", (op >> 6) & 15);
	goto done;
LDCR: STCR:
	pc = disasm_Ts(pc, op, 2);
	sprintf(asm_text+strlen(asm_text), ",%d", (op >> 6) & 15 ?: 16);
	goto done;

JMP: JLT: JLE: JEQ: JHE: JGT: JNE: JNC: JOC: JNO: JL: JH: JOP:
	sprintf(asm_text+strlen(asm_text), ">%04X", pc + 2 + 2 * (s8)(op & 0xff));
	sprintf(reg_text, "ST=%s%s%s%s%s",
		tst_EQ() ? "EQ " : "",
		tst_GT() ? "A> " : "",
		tst_H()&&tst_HE() ? "L> " : "",
		tst_C() ? "C " : "",
		tst_OV() ? "OV " : "");
	goto done;
SBO: SBZ: TB:
	sprintf(asm_text+strlen(asm_text), "%d", op & 0xff);
	goto done;
SRA: SRL: SLA: SRC:
	sprintf(asm_text+strlen(asm_text), "R%d,", op & 15);
	sprintf(asm_text+strlen(asm_text), (op & 0x00f0) ? "%d" : "R0", (op >> 4) & 15);
	goto done;
BLWP: B: BL:
	pc = disasm_Bs(pc, op, 2);
	goto done;
X: CLR: NEG: INV: INC: INCT: DEC: DECT: SWPB: SETO: ABS:
	pc = disasm_Ts(pc, op, 2);
	goto done;
LI: AI: ANDI: ORI: CI:
	sprintf(reg_text, "R%d=%04X",
		op & 15, safe_r(wp + (op & 15)*2));
	pc += 2;
	sprintf(asm_text+strlen(asm_text), "R%d,>%04X", op & 15, safe_r(pc));
	goto done;
STWP: STST:
	sprintf(asm_text+strlen(asm_text), "R%d", op & 15);
	goto done;
IDLE: RSET: RTWP: CKON: CKOF: LREX:
	goto done;
LWPI: LIMI:
	pc += 2;
	sprintf(asm_text+strlen(asm_text), ">%04X", safe_r(pc));
	goto done;

done:
	if (cycles) {
		int n = 50;
		char tmp[10];
		int i = strlen(asm_text);
		sprintf(tmp, "(%d)", cycles);
		memset(asm_text + i, ' ', n - i);
		strcpy(asm_text + n - strlen(tmp), tmp);
	}
	sprintf(asm_text+strlen(asm_text), "\n");
	//sprintf(asm_text+strlen(asm_text), "\t\t%s\n", reg_text);
	//sprintf(asm_text+strlen(asm_text), "\t\t(%d)\n", disasm_cyc);
	int ret = pc+2 - start_pc;
	while (start_pc != pc) {
		start_pc += 2;
		sprintf(asm_text+strlen(asm_text), "           %04X\n", safe_r(start_pc));
	}
	cyc = save_cyc;
	return ret;
}



#ifdef CPU_TEST

// assemble src, returns number of words written to dst
static int as(const char *src, u16 *dst)
{
	struct {
		char *str;
		u16 base;
		int fmt;
	} fmts[] = {
		{"LI", 0x200, 8}, {"AI", 0x220, 8}, {"ANDI", 0x240, 8}, {"ORI", 0x260, 8}, {"CI", 0x280, 8},
		{"STWP", 0x2A0, 10}, {"STST", 0x2C0, 10}, {"LWPI", 0x2E0, 10}, {"LIMI", 0x300, 10},
		{"IDLE", 0x340, 7}, {"RSET", 0x360, 7}, {"RTWP", 0x380, 7}, {"CKON", 0x3A0, 7}, {"CKOF", 0x3C0, 7}, {"LREX", 0x3E0, 7},
		{"BLWP", 0x400, 6}, {"B", 0x440, 6}, {"X", 0x480, 6}, {"CLR", 0x4C0, 6}, {"NEG", 0x500, 6},
		{"INV", 0x540, 6}, {"INC", 0x580, 6}, {"INCT", 0x5C0, 6}, {"DEC", 0x600, 6}, {"DECT", 0x640, 6},
		{"BL", 0x680, 6}, {"SWPB", 0x6C0, 6}, {"SETO", 0x700, 6}, {"ABS", 0x740, 6},
		{"SRA", 0x800, 5}, {"SRL", 0x900, 5}, {"SLA", 0xA00, 5}, {"SRC", 0xB00, 5},
		{"JMP", 0x1000, 2}, {"JLT", 0x1100, 2}, {"JLE", 0x1200, 2}, {"JEQ", 0x1300, 2},
		{"JHE", 0x1400, 2}, {"JGT", 0x1500, 2}, {"JNE", 0x1600, 2}, {"JNC", 0x1700, 2},
		{"JOC", 0x1800, 2}, {"JNO", 0x1900, 2}, {"JL", 0x1A00, 2}, {"JH", 0x1B00, 2},
		{"JOP", 0x1C00, 2}, {"SBO", 0x1D00, 2}, {"SBZ", 0x1E00, 2}, {"TB", 0x1F00, 2},
		{"COC", 0x2000, 3}, {"CZC", 0x2400, 3}, {"XOR", 0x2800, 3}, {"XOP", 0x2C00, 9},
		{"LDCR", 0x00, 4}, {"STCR", 0x3400, 4}, {"MPY", 0x3800, 9}, {"DIV", 0x3C00, 9},
		{"SZC", 0x4000, 1}, {"SZCB", 0x5000, 1}, {"S", 0x6000, 1}, {"SB", 0x7000, 1},
		{"C", 0x8000, 1}, {"CB", 0x9000, 1}, {"A", 0xA000, 1}, {"AB", 0xB000, 1},
		{"MOV", 0xC000, 1}, {"MOVB", 0xD000, 1}, {"SOC", 0xE000, 1}, {"SOCB", 0xF000, 1},
	};
	int i;
	for (i = 0; i < ARRAY_SIZE(fmts); i++) {
		int len = strlen(fmts[i].str);
		int n = 0;
		if (strncmp(src, fmts[i].str, len)  != 0) continue;
		if (src[len] != 0 && src[len] != ' ') continue;
		src += len; // skip word
		while (src[0] == ' ') src++; // skip spaces
		dst[n++] = fmts[i].base;
		switch (fmts[i].fmt) {
			// TODO
		}
		return n;
	}
	return 0;
}


// Stubs for testing
#include <stdarg.h>
int breakpoint_read(u16 address) { return 0; }
int breakpoint_write(u16 address) { return 0; }
int debug_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int ret = vfprintf(stderr, fmt, ap);
	va_end(ap);
	return ret;
}
u8 cru_r(u16 bit) { return 1; }
void cru_w(u16 bit, u8 value) {}
void unhandled(u16 pc, u16 op) {}
int get_cart_bank(void) { return 0; }
#ifdef ENABLE_DEBUGGER
int debug_break = DEBUG_RUN;
#endif

static u16 fast_ram[128] = {}; // 256 bytes at 8000-80ff,repeated at 8100,8200,8300
static u16 *ram = NULL; // 32k RAM or SAMS
static unsigned int ram_size = 0; // in bytes

/******************************************
 * 8000-83FF  fast RAM                    *
 ******************************************/

static u16 ram_8300_r(u16 address)
{
	// fast RAM, incompletely decoded at 8000, 8100, 8200, 8300
	add_cyc(2);
	return fast_ram[(address & 0xfe) >> 1];
}

static void ram_8300_w(u16 address, u16 value)
{
	add_cyc(2);
	fast_ram[(address & 0xfe) >> 1] = value;
}

void generate_test_code(u16 *dest)
{
	FILE *f = fopen("test/timing.asm", "wb");
	int wp, i, j, k, l;

	if (!f) return;
	fprintf(f, //"       aorg >A000\n"
	           //"       data >0000,>A000,end->A000\n"
		   "       aorg >6000\n"
		   "       data >aa00,>0100,>0000\n"
		   "       data prglst,>0000\n"
		   "prglst data >0000,start\n"
		   "       stri 'TEST'\n"
		   "       even\n"
		   "start\n"
		   );


	for (wp = 0; wp < 2; wp++) {
		const char *srcs[] = {"r0","*r8","*r9","*r8+","*r9+","@2(r8)","@2(r9)","@>201e","@>831e"};
		const char *dsts[] = {"r1","*r8","*r9","*r8+","*r9+","@2(r8)","@2(r9)","@>201e","@>831e"};
		const char *sspd[] = {"","s+","s-","s+","s-","s+","s-","",""};
		const char *dspd[] = {"","d+","d-","d+","d-","d+","d-","",""};

		fprintf(f, "       lwpi >%04x\n"
			   "       li r8,>2020\n",
				wp == 0 ? 0x8300 : 0x2000);

		for (i = 0; i < 10; i++) {
			const char *isns[] = {"szc","szcb","s","sb","c","cb","a","ab","soc","socb"};
			const char *isn = isns[i];

			fputs("       li r9,>8320\n", f);
			for (j = 0; j < ARRAY_SIZE(srcs); j++) {
				for (k = 0; k < ARRAY_SIZE(dsts); k++) {
					fprintf(f, "       %s %s,%s%s%s%s\n", isn,
						srcs[j], dsts[k],
						sspd[j][0]||dspd[k][0] ? " ;: " : "",
						sspd[j], dspd[k]);
				}
			}
		}
		for (i = 0; i < 3; i++) {
			const char *isns[] = {"coc","czc","xor"};
			const char *isn = isns[i];
			for (j = 0; j < ARRAY_SIZE(srcs); j++) {
				fprintf(f, "       %s %s,%s%s%s%s\n", isn,
					srcs[j], "r1",
					sspd[j][0] ? " ;: " : "",
					sspd[j], "");
			}
		}
		for (i = 0; i < 4; i++) {
			const char *isns[] = {"sra","srl","sla","src"};
			const char *isn = isns[i];
			for (j = 1; j < 16; j++) {
				fprintf(f, "       %s r0,%d\n", isn, j);
			}
		}
		for (i = 0; i < 10; i++) {
			const char *isns[] = {"clr","neg","inv","inc","inct","dec","dect","swpb","seto","abs"};
			const char *isn = isns[i];
			for (j = 0; j < ARRAY_SIZE(srcs); j++) {
				fprintf(f, "       %s %s %s%s\n", isn,
					srcs[j],
					sspd[j][0] ? " ;: " : "",
					sspd[j]);
			}
		}
		for (i = 0; i < 5; i++) {
			const char *isns[] = {"li","ai","andi","ori","ci"};
			const char *isn = isns[i];
			fprintf(f, "       %s r0,>1234\n", isn);
		}
	}
	fputs("       b @$+4\n"
	      "       li r0,$+6\n"
	      "       b *r0\n"
	      "       jmp $\n"
		"end\n", f);
	fclose(f);
	if (system("make -B -C test timingc.bin")) return;

	f = fopen("test/timingc.bin","rb");
	if (!f) abort();
	u16 *pcptr = dest + 6;
	while (!feof(f)) {
		unsigned char b[2];
		if (fread(b, 1, 2, f) != 2) break;
		*dest++ = (b[0] << 8) | b[1];
	}
	fclose(f);
	gPC = *pcptr;

	f = fopen("test/timingc.lst","rb");
	if (!f) abort();
	while (!feof(f)) {
		char line[256];
		int pc, cc;
		if (!fgets(line, 256, f)) break;
		if (strlen(line) < 17) continue;
		pc = strtol(line+5, NULL, 16); // program counter for this line
		if (pc != gPC) continue;
		cc = strtol(line+16, NULL, 10); // cycle count for this line

		cyc = 0; single_step(); // run 1 instruction and get cycle count
		if (cyc != cc) {
			disasm(pc, cyc);
			printf("%s", asm_text);
			printf("expected %d     WP=%04x R8=%04x R9=%04X\n%s\n", cc,
				gWP, reg_r(gWP, 8),reg_r(gWP, 9),
				line);
			//break;
		}
	}
	fclose(f);
}



int main(int argc, char *argv[])
{
	ram_size = 32 * 1024; // 32K 
	ram = calloc(ram_size, 1);
	u16 *cart = calloc(8, 1024); // 8K

	// test ram 2000-3fff
	set_mapping(0x2000, 0x2000, map_r, map_w, ram);
	set_mapping(0xa000, 0x6000, map_r, map_w, ram+0x1000/*words*/);
	set_mapping(0x6000, 0x2000, map_r, map_w, cart);

	// memory mapped devices 8000-9fff
	set_mapping(0x8000, 0x400, ram_8300_r, ram_8300_w, NULL);


	gPC = 0xa000;

	generate_test_code(cart); // load and run program at >6000


	return 0;
}

#endif
