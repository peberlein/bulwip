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
#define _POSIX_C_SOURCE 1
#include <stdio.h>
#include <string.h>
#include <stdlib.h>



extern int debug_log(const char *fmt, ...);
extern int print_asm(const char *fmt, ...);


typedef unsigned int u32;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned char u8;
typedef signed char s8;


#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))

// program counter, workspace pointer, status flags
static u16 gPC, gWP; // st is below with status functions
static int cyc = 0; // cycle counter (cpu runs until > 0)
static int interrupt_level = 0; // interrupt level + 1  (call interrupt_check() after modifying)

u16 get_pc(void) { return gPC; }
u16 get_wp(void) { return gWP; }
u16 get_st(void); // defined below with status functions
int add_cyc(int add) { cyc += add; return cyc; }

// Setting en will allow the next emu() to return after a single instruction
// Clearing en will restore the number of saved cycles before returning
void cpu_break(int en)
{
	static int saved_cyc = 0;
	if (en) {
		saved_cyc += cyc;
		cyc = 0;
	} else {
		cyc += saved_cyc;
		saved_cyc = 0;
	}
}


// external CRU functions
extern u8 cru_r(u16 bit);
extern void cru_w(u16 bit, u8 value);



/******************************************
 * Memory page mapping functions          *
 ******************************************/


// Use 12 for 4K pages, or 10 for 1K pages (0x400) or 8 for 256B pages
//#define MAP_SHIFT 12
#define MAP_SHIFT 8
#define PAGE_SIZE (1 << MAP_SHIFT)
#define PAGE_MASK (PAGE_SIZE-1)
#define PAGES_IN_64K (1<<(16-MAP_SHIFT))

static struct {
	u16 (*read)(u16);         // read function
	u16 (*safe_read)(u16);    // read function without side-effects (but may increment cyc)
	void (*write)(u16, u16);  // write function
	u16 *mem;                 // memory reference
} map[PAGES_IN_64K]; // N (1<<MAP_SHIFT) banks


void change_mapping(int base, int size, u16 *mem)
{
	int i;
	for (i = 0; i < size; i += PAGE_SIZE) {
		map[base >> MAP_SHIFT].mem = mem + (i>>1);
		base += PAGE_SIZE;
	}
}

void set_mapping_safe(int base,
	u16 (*read)(u16),
	u16 (*safe_read)(u16),
	void (*write)(u16, u16),
	u16 *mem)
{
	int i;
	base <<= (12-MAP_SHIFT);
	for (i = 0; i < (1 << (12-MAP_SHIFT)); i++) {
		map[base+i].read = read;
		map[base+i].safe_read = safe_read;
		map[base+i].write = write;
		map[base+i].mem = mem ? mem + (i << (MAP_SHIFT-1)) : NULL;
		//printf("map page=%x address=%p\n", base+i, map[base+i].mem);
	}
}

void set_mapping(int base,
	u16 (*read)(u16),
	void (*write)(u16, u16),
	u16 *mem)
{
	set_mapping_safe(base, read, read, write, mem);
}



/******************************************
 * Memory accessor functions              *
 ******************************************/

// These may be overridden by debugger for breakpoints/watchpoints
//static u16 (*iaq_func)(u16); // instruction acquisition

static inline u16 mem_r(u16 address)
{
	u16 value = map[address >> MAP_SHIFT].read(address);
	return value;
}

u16 safe_r(u16 address)
{
	u16 value = map[address >> MAP_SHIFT].safe_read(address);
	return value;
}

static inline u16 mem_w(u16 address, u16 value)
{
	map[address >> MAP_SHIFT].write(address, value);
	return value;
}

static inline u16 reg_r(u16 wp, u8 reg)
{
	return mem_r(wp + 2 * reg);
}

static inline u16 reg_w(u16 wp, u8 reg, u16 value)
{
	return mem_w(wp + 2 * reg, value);
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

	if (map[page].mem == NULL) {
		debug_log("no memory mapped at %04X (read)\n", address);
		return 0;
	}
	return map[page].mem[offset >> 1];
}

void map_w(u16 address, u16 value)
{
	u8  page = address >> MAP_SHIFT;
	u16 offset = address & PAGE_MASK;
	if (map[page].mem == NULL) {
		debug_log("no memory mapped at %04X (write, %04X)\n", address, value);
		return;
	}
	map[page].mem[offset >> 1] = value;
	return;
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

#define always_inline inline __attribute((always_inline))

u16 get_st(void) { return ((u16)st_flg << 8) | st_int; }
static always_inline void set_st(u16 new_st) { st_flg = new_st >> 8; st_int = new_st & ST_IM; }
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

#define USE_ZERO_TABLE
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

extern void unhandled(u16 pc, u16 op);

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
	return (op >> 4) & 15 ?: reg_r(wp, 0) & 15 ?: 16;
}



/*************************************************
 * Ts source and Td destination operand decoding *
 *************************************************/

// source returned as value (indirects are followed)
static always_inline u16 Ts(u16 op, u16 *pc, u16 wp)
{
	u16 val, addr;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx
		val = reg_r(wp, reg);
		break;
	case 1: // *Rx
		val = mem_r(reg_r(wp, reg));
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		addr = reg_r(wp, reg);
		val = mem_r(mem_r(*pc) + (reg ? addr : 0));
		*pc += 2;
		cyc += 4;
		break;
	case 3: // *Rx+
		addr = reg_r(wp, reg);
		val = mem_r(addr);   // read value, then update
		reg_w(wp, reg, addr + 2);
		cyc += 4;
		break;
	}
	return val;
}

// source returned as byte value (in high byte, low byte is zero)
// (indirects are followed)
static always_inline u16 TsB(u16 op, u16 *pc, u16 wp)
{
	u16 val, addr;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx   2c
		return reg_r(wp, reg) & 0xff00;
	case 1: // *Rx  6c
		addr = reg_r(wp, reg);
		val = mem_r(addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0  10c
		addr = reg_r(wp, reg);
		addr = mem_r(*pc) + (reg ? addr : 0);
		*pc += 2;
		val = mem_r(addr);
		cyc += 4;
		break;
	case 3: // *Rx+   10c
		addr = reg_r(wp, reg);
		val = mem_r(addr);
		reg_w(wp, reg, addr + 1);
		cyc += 2;
		break;
	}
	return (addr & 1) ? val << 8 : val & 0xff00;
}

struct val_addr {
	u16 val, addr;
};

static always_inline struct val_addr Td(u16 op, u16 *pc, u16 wp)
{
	struct val_addr va;
	u16 reg = op & 15;

	switch ((op >> 4) & 3) {
	case 0: // Rx  2c
		va.val = reg_r(wp, reg);
		va.addr = wp + 2 * reg;
		break;
	case 1: // *Rx  6c
		va.addr = reg_r(wp, reg);
		va.val = mem_r(va.addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0  10c
		va.addr = reg_r(wp, reg);
		va.addr = mem_r(*pc) + (reg ? va.addr : 0);
		*pc += 2;
		va.val = mem_r(va.addr);
		cyc += 4;
		break;
	case 3: // *Rx+   10c
		va.addr = reg_r(wp, reg);
		va.val = mem_r(va.addr);
		//reg_w(wp, reg, va.addr + 2); // Post-increment must be done later
		cyc += 4;
		break;
	}
	return va;
}

static always_inline struct val_addr TdB(u16 op, u16 *pc, u16 wp)
{
	struct val_addr va;
	u16 reg = op & 15;

	va.addr = reg_r(wp, reg);
	switch ((op >> 4) & 3) {
	case 0: // Rx
		va.val = va.addr;
		va.addr = wp + 2 * reg;
		break;
	case 1: // *Rx
		va.val = mem_r(va.addr);
		cyc += 2;
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		va.addr = mem_r(*pc) + (reg ? va.addr : 0);
		*pc += 2;
		va.val = mem_r(va.addr);
		cyc += 4;
		break;
	case 3: // *Rx+
		va.val = mem_r(va.addr);
		//reg_w(wp, reg, va.addr + 1); // Post-increment must be done later
		cyc += 2;
		break;
	}
	return va;
}

static always_inline void Td_post_increment(u16 op, u16 wp, struct val_addr va, int bytes)
{
	if (((op >> 4) & 3) == 3) {
		// post increment
		reg_w(wp, op & 15, va.addr + bytes);
	}
}

static always_inline void mem_w_Td(u16 op, u16 wp, struct val_addr va)
{
	mem_w(va.addr, va.val);
	Td_post_increment(op, wp, va, 2);
}

static always_inline void mem_w_TdB(u16 op, u16 wp, struct val_addr va)
{
	mem_w(va.addr, va.val);
	Td_post_increment(op, wp, va, 1);
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

int disasm(u16 pc); // forward


// idea for faster indirect memory read/write
// keep a cache of read/write functions for all 16 registers
// cache entries default to a function that updates the cache
// 16 default functions, one for each register
// changing workspace pointer resets the function cache
// writing to a register resets the cache entry
// writing to memory WP to WP+31 resets the cache entry


#if 1
// instruction opcode decoding using count-leading-zeroes (clz)

void emu(void)
{
	u16 op, pc = gPC, wp = gWP;
decode_op:
	if (cyc > 0) {
		gPC = pc;
		gWP = wp;
		return;
	}
decode_op_now:
	if (0/*trace*/) {
		int save_cyc = cyc;
		disasm(pc);
		cyc = save_cyc;
	}
	op = mem_r(pc);
	pc += 2;
execute_op:
	cyc += 6; // base cycles
	if (op < 0x0200) goto UNHANDLED;
	switch (__builtin_clz(op << 16)) {
	case 0: case 1: {  // opcodes 0x4000 .. 0xffff
		u16 ts, reg;
		struct val_addr td;
		if (op & 0x1000) { // byte op
			ts = TsB(op, &pc, wp);
			op >>= 6;
			td = TdB(op, &pc, wp);
			switch ((op >> 7) & 7) {
			case 0: goto UNHANDLED;
			case 1: goto UNHANDLED;
			case 2: // SZCB
				if (td.addr & 1) {
					td.val &= ~(ts >> 8);
					status_parity(status_zero(td.val << 8));
				} else {
					td.val &= ~(ts & 0xff00);
					status_parity(status_zero(td.val & 0xff00));
				}
				mem_w_TdB(op, wp, td);
				goto decode_op;
			case 3: // SB
				if (td.addr & 1) {
					td.val = (td.val & 0xff00) | status_parity(sub(td.val << 8, ts) >> 8);
				} else {
					td.val = (td.val & 0x00ff) | status_parity(sub(td.val & 0xff00, ts));
				}
				mem_w_TdB(op, wp, td);
				goto decode_op;
			case 4: // CB
				if (td.addr & 1) {
					status_arith(status_parity(ts), td.val << 8);
				} else {
					status_arith(status_parity(ts), td.val & 0xff00);
				}
				goto decode_op;
			case 5: // AB
				if (td.addr & 1) {
					td.val = (td.val & 0xff00) | status_parity(add(td.val << 8, ts) >> 8);
				} else {
					td.val = (td.val & 0x00ff) | status_parity(add(td.val & 0xff00, ts));
				}
				mem_w_TdB(op, wp, td);
				goto decode_op;
			case 6: // MOVB
				if (td.addr & 1) {
					td.val = (td.val & 0xff00) | status_parity(status_zero(ts) >> 8);
				} else {
					td.val = (td.val & 0x00ff) | status_parity(status_zero(ts));
				}
				mem_w_TdB(op, wp, td);
				goto decode_op;
			case 7: // SOCB
				if (td.addr & 1) {
					td.val |= (ts >> 8);
					status_parity(status_zero(td.val << 8));
				} else {
					td.val |= ts;
					status_parity(status_zero(td.val & 0xff00));
				}
				mem_w_TdB(op, wp, td);
				goto decode_op;
			}
		} else { // word op
			ts = Ts(op, &pc, wp);
			op >>= 6;
			td = Td(op, &pc, wp);
			switch ((op >> 7) & 7) {
			case 0: goto UNHANDLED;
			case 1: goto UNHANDLED;
			case 2: // SZC
				td.val = status_zero(td.val & ~ts);
				mem_w_Td(op, wp, td);
				goto decode_op;
			case 3: // S
				//mem_w(td.addr, sub(td.val, ts));
				td.val = sub(td.val, ts);
				mem_w_Td(op, wp, td);
				goto decode_op;
			case 4: // C
				status_arith(ts, td.val);
				Td_post_increment(op, wp, td, 2);
				goto decode_op;
			case 5: // A
				td.val = add(td.val, ts);
				mem_w_Td(op, wp, td);
				goto decode_op;
			case 6: // MOV
				td.val = status_zero(ts);
				mem_w_Td(op, wp, td);
				goto decode_op;
			case 7: // SOC
				td.val = status_zero(td.val | ts);
				mem_w_Td(op, wp, td);
				goto decode_op;
			}
		}
		__builtin_unreachable();
		}
	case 2:   // opcodes 0x2000 .. 0x3fff
		switch ((op >> 10) & 7) {
		case 0: { // COC
			u16 ts = Ts(op, &pc, wp);
			u16 td = reg_r(wp, (op >> 6) & 15);
			if ((ts & td) == ts) set_EQ(); else clr_EQ();
			goto decode_op;
			}
		case 1: { // CZC
			u16 ts = Ts(op, &pc, wp);
			u16 td = reg_r(wp, (op >> 6) & 15);
			if (!(ts & td)) set_EQ(); else clr_EQ();
			goto decode_op;
			}
		case 2: { // XOR
			u16 ts = Ts(op, &pc, wp);
			u16 td = reg_r(wp, (op >> 6) & 15);
			reg_w(wp, (op >> 6) & 15, status_zero(ts ^ td));
			goto decode_op;
			}
		case 3: { // XOP
			struct val_addr td = Td(op, &pc, wp); // source
			u8 reg = (op >> 6) & 15; // XOP number usually 1 or 2
			u16 ts = mem_r(0x0040 + (reg << 2)); // new WP

			mem_w(ts + 2*11, td.addr); // gAS copied to R11
			Td_post_increment(op, wp, td, 2);
			mem_w(ts + 2*13, wp); // WP to R13
			mem_w(ts + 2*14, pc); // PC to R14
			mem_w(ts + 2*15, get_st()); // ST to R15
			pc = mem_r(0x0042 + (reg << 2));
			wp = ts;
			set_X();
			//printf("XOP %04x %d  PC=%04x WP=%04x\n", td.val, reg, pc, wp);

			goto decode_op_now; // next instruction cannot be interrupted
			}
		case 4: { // LDCR
			u8 idx, c = ((op >> 6) & 15) ?: 16;
			u16 ts, reg = (reg_r(wp, 12) & 0x1ffe) >> 1;
			if (c <= 8) {
				ts = TsB(op, &pc, wp) >> 8;
				status_parity(ts);
			} else {
				ts = Ts(op, &pc, wp);
			}
			//printf("LDCR c=%d reg=%d ts=%x\n", c, reg, ts);
			for (idx = 0; idx < c; idx++)
				cru_w(reg + idx, (1 << idx) & ts ? 1 : 0);
			status_zero(ts);
			goto decode_op;
			}
		case 5: { // STCR
			struct val_addr td;
			u8 idx, c = ((op >> 6) & 15) ?: 16;
			u16 reg = (reg_r(wp, 12) & 0x1ffe) >> 1;
			if (c <= 8) {
				td = TdB(op, &pc, wp);
				td.val &= (td.addr & 1) ? 0xff00 : 0x00ff;
				u16 base = (td.addr & 1) ? 1 : 0x100;
				for (idx = 0; idx < c; idx++) {
					if (cru_r(reg + idx))
						td.val |= (base << idx);
				}
				mem_w_TdB(op, wp, td);
				//if (reg == 3 /*&& (va.val&0xff00) != 0xff00*/) {
				//	printf("STCR %d reg=%d val=%04x pc=%x kb=%d R4=%04x\n", c, reg, va.val, pc, keyboard_row,
				//			fast_ram[(wp-0x8300)/2+4]);
				//	
				//}
				status_parity(status_zero(td.val & 0xff00));
			} else {
				td = Td(op, &pc, wp);
				td.val = 0;
				for (idx = 0; idx < c; idx++) {
					if (cru_r(reg + idx))
						td.val |= (1 << idx);
				}
				//printf("STCR %d reg=%d val=%04x pc=%x\n", c, reg, va.val, pc);
				mem_w_Td(op, wp, td);
				status_zero(td.val);
			}
			//printf("STCR reg=%d %x %d  %x\n", reg, va.addr, c, va.val);
			// TODO status bits
			goto decode_op;
			}
		case 6: { // MPY
			u32 val = Ts(op, &pc, wp);
			u8 reg = (op >> 6) & 15;
			//debug_log("MPY %04X x %04X = %08X\n", 
			//	val, reg_r(wp, reg), val * reg_r(wp, reg));
			val *= reg_r(wp, reg);
			reg_w(wp, reg, val >> 16);
			reg_w(wp, reg+1, val & 0xffff);
			goto decode_op;
			}
		case 7: { // DIV
			u32 val;
			u16 ts = Ts(op, &pc, wp);
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
			goto decode_op;
			}
		}
		__builtin_unreachable();
	case 3: // opcodes 0x1000 .. 0x1fff
		switch ((op >> 8) & 15) {
		case 0: JMP: cyc += 2; pc += 2 * (s8)(op & 0xff); goto decode_op;
		case 1: JLT: if (tst_LT()) goto JMP; goto decode_op;
		case 2: JLE: if (tst_LE()) goto JMP; goto decode_op;
		case 3: JEQ: if (tst_EQ()) goto JMP; goto decode_op;
		case 4: JHE: if (tst_HE()) goto JMP; goto decode_op;
		case 5: JGT: if (tst_GT()) goto JMP; goto decode_op;
		case 6: JNE: if (!tst_EQ()) goto JMP; goto decode_op;
		case 7: JNC: if (!tst_C()) goto JMP; goto decode_op;
		case 8: JOC: if (tst_C()) goto JMP; goto decode_op;
		case 9: JNO: if (!tst_OV()) goto JMP; goto decode_op;
		case 10: JL: if (tst_L()) goto JMP; goto decode_op;
		case 11: JH: if (tst_H()) goto JMP; goto decode_op;
		case 12: JOP: if (tst_OP()) goto JMP; goto decode_op;
		case 13: SBO: cru_w((op & 0xff) + ((reg_r(wp, 12)&0x1ffe) >> 1), 1); goto decode_op;
		case 14: SBZ: cru_w((op & 0xff) + ((reg_r(wp, 12)&0x1ffe) >> 1), 0); goto decode_op;
		case 15: TB: status_equal(cru_r((op & 0xff) + ((reg_r(wp, 12) & 0x1ffe) >> 1)), 0); goto decode_op;
		}
		__builtin_unreachable();
	case 4: { // opcodes 0x0800 .. 0x0fff  shifts
		u16 reg = reg_r(wp, op & 15);
		u8 count = shift_count(op, wp);
		// TODO these need cycle counts!
		switch ((op >> 8) & 3) {
		case 0: SRA:
			if (reg & (1 << (count-1))) set_C(); else clr_C();
			reg_w(wp, op & 15, status_zero(((s16)reg) >> count));
			goto decode_op;
		case 1: SRL:
			if (reg & (1 << (count-1))) set_C(); else clr_C();
			reg_w(wp, op & 15, status_zero(reg >> count));
			goto decode_op;
		case 2: SLA:
			if (reg & (0x8000 >> (count-1))) set_C(); else clr_C();
			reg_w(wp, op & 15, status_zero(reg << count));
			// overflow if MSB changes during shift
			if (count == 16) {
			  if (reg) set_OV(); else clr_OV();
			} else {
			  u16 ov_mask = 0xffff << (15 - count);
			  reg &= ov_mask;
			  if (reg != 0 && reg != ov_mask) set_OV(); else clr_OV();
			}
			goto decode_op;
		case 3: SRC:
			if (reg & (1 << (count-1))) set_C(); else clr_C();
			reg_w(wp, op & 15, status_zero((reg << (16-count))|(reg >> count)));
			goto decode_op;
		}
		__builtin_unreachable();
		}
	case 5: { // opcodes 0x0400 .. 0x07ff
		struct val_addr td;
		switch ((op >> 6) & 15) {
		case 0: BLWP:
			td = Td(op, &pc, wp);
			//debug_log("OLD pc=%04X wp=%04X st=%04X  NEW pc=%04X wp=%04X\n", pc, wp, st, safe_r(va.addr+2), va.val);
			mem_w(td.val + 2*13, wp);
			mem_w(td.val + 2*14, pc);
			mem_w(td.val + 2*15, get_st());
			pc = mem_r(td.addr + 2);
			wp = td.val;
			Td_post_increment(op, wp, td, 2);
			goto decode_op_now; // next instruction cannot be interrupted
		case 1: B:    td = Td(op, &pc, wp); Td_post_increment(op, wp, td, 2); pc = td.addr; goto decode_op;
		case 2: X:    td = Td(op, &pc, wp); Td_post_increment(op, wp, td, 2); op = td.val; /*printf("X op=%x pc=%x\n", op, pc);*/ goto execute_op;
		case 3: CLR:  td = Td(op, &pc, wp); td.val = 0; mem_w_Td(op, wp, td); goto decode_op;
		case 4: NEG:  td = Td(op, &pc, wp); td.val = sub(0, td.val); mem_w_Td(op, wp, td); goto decode_op;
		case 5: INV:  td = Td(op, &pc, wp); td.val = status_zero(~td.val); mem_w_Td(op, wp, td); goto decode_op;
		case 6: INC:  td = Td(op, &pc, wp); td.val = add(td.val,1); mem_w_Td(op, wp, td); goto decode_op;
		case 7: INCT: td = Td(op, &pc, wp); td.val = add(td.val,2); mem_w_Td(op, wp, td); goto decode_op;
		case 8: DEC:  td = Td(op, &pc, wp); td.val = sub(td.val,1); mem_w_Td(op, wp, td); goto decode_op;
		case 9: DECT: td = Td(op, &pc, wp); td.val = sub(td.val,2); mem_w_Td(op, wp, td); goto decode_op;
		case 10: BL:   td = Td(op, &pc, wp); Td_post_increment(op, wp, td, 2); reg_w(wp, 11, pc); pc = td.addr; goto decode_op;
		case 11: SWPB: td = Td(op, &pc, wp); td.val = swpb(td.val); mem_w_Td(op, wp, td); goto decode_op;
		case 12: SETO: td = Td(op, &pc, wp); td.val = 0xffff; mem_w_Td(op, wp, td); goto decode_op;
		case 13: ABS:
			td = Td(op, &pc, wp); status_zero(td.val);
			//st &= ~(ST_OV | ST_C); 
			clr_OV();
			clr_C(); // carry is not listed affected in manual, but it is
			if (td.val & 0x8000) {
				cyc += 2;
				if (td.val == 0x8000)
					set_OV();
				else {
					td.val = -td.val;
				}
			}
			mem_w_Td(op, wp, td);
			goto decode_op;
		case 14: goto UNHANDLED;
		case 15: goto UNHANDLED;
		}
		__builtin_unreachable();
		}
	case 6: { // opcodes 0x0200 .. 0x03ff
		u8 reg = op & 15;
		cyc -= 2;
		switch ((op >> 5) & 15) {
		case 0: LI: reg_w(wp, reg, (reg_r(wp, reg), status_zero(mem_r(pc)))); pc += 2; goto decode_op;
		case 1: AI: reg_w(wp, reg, add(reg_r(wp, reg), mem_r(pc))); pc += 2; goto decode_op;
		case 2: ANDI: reg_w(wp, reg, status_zero(reg_r(wp, reg) & mem_r(pc))); pc += 2; goto decode_op;
		case 3: ORI: reg_w(wp, reg, status_zero(reg_r(wp, reg) | mem_r(pc))); pc += 2; goto decode_op;
		case 4: CI: status_arith(reg_r(wp, reg), mem_r(pc)); pc += 2; goto decode_op;
		case 5: STWP: reg_w(wp, reg, wp); goto decode_op;
		case 6: STST: reg_w(wp, reg, get_st()); goto decode_op;
		case 7: LWPI: wp = mem_r(pc); pc += 2; goto decode_op;
		case 8: LIMI: set_IM(mem_r(pc) & 15); pc += 2; gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; goto decode_op;
		case 9: goto UNHANDLED;
		case 10: IDLE: debug_log("IDLE not implemented\n");/* TODO */ goto decode_op;
		case 11: RSET: debug_log("RSET not implemented\n"); /* TODO */ goto decode_op;
		case 12: RTWP: set_st(reg_r(wp, 15)); pc = reg_r(wp, 14); wp = reg_r(wp, 13); gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; goto decode_op;
		case 13: CKON: debug_log("CKON not implemented\n");/* TODO */ goto decode_op;
		case 14: CKOF: debug_log("CKOF not implemented\n");/* TODO */ goto decode_op;
		case 15: LREX: debug_log("LREX not implemented\n");/* TODO */ goto decode_op;
		}
		__builtin_unreachable();
		}
	default:
		printf("%04x: %04x  %d\n", pc, op, (int)__builtin_clz(op));
		UNHANDLED:
		unhandled(pc, op);
		goto decode_op;
	}
	__builtin_unreachable();
}

#else
void emu(void)
{
	u16 op, pc = gPC, wp = gWP;

#define NEXT goto decode_op
	static const int jt1[]={L8(C,CB,A,AB,MOV,MOVB,SOC,SOCB)}; // 14,14,14,14,14,14,14,14
	static const int jt2[]={L4(SZC,SZCB,S,SB)}; // 14,14,14,14
	static const int jt3[]={L8(COC,CZC,XOR,XOP,LDCR,STCR,MPY,DIV)}; // 14,14,14,36,20+2C,42/44/58/60,52,92-124
	static const int jt4[]={L8(JMP,JLT,JLE,JEQ,JHE,JGT,JNE,JNC),L8(JOC,JNO,JL,JH,JOP,SBO,SBZ,TB)}; // 8-10, ... ,12,12,12
	static const int jt5[]={L4(SRA,SRL,SLA,SRC)}; // 12+2C/52/20+2N
	static const int jt6[]={L8(BLWP,B,X,CLR,NEG,INV,INC,INCT),L8(DEC,DECT,BL,SWPB,SETO,ABS,BAD,BAD)}; // 26,8,8,10,12,
	static const int jt7[]={L8(LI,AI,ANDI,ORI,CI,STWP,STST,LWPI),L8(LIMI,BAD,IDLE,RSET,RTWP,CKON,CKOF,LREX)};
	static const int * jt[] = {jt1, jt2, jt3, jt4, jt5, jt6, jt7};

decode_op:
	if (cyc > 0) {
		gPC = pc;
		gWP = wp;
		return;
	}
decode_op_now:
	if (0/*trace*/) {
		int save_cyc = cyc;
		disasm(pc);
		cyc = save_cyc;
	}
	op = mem_r(pc);
	pc += 2;
execute_op:
	{
	u8 idx = __builtin_clz(op)-16;
	if (idx >= ARRAY_SIZE(jt)) {
		BAD:
		unhandled(pc, op);
		NEXT;
	}
	cyc += decode[idx].cycles;
	u8 subidx = (op >> decode[idx].shift) & decode[idx].mask;
	goto *(&&C + jt[idx][subidx]);
	}


	{
	u16 ts, reg;
	struct val_addr td;

SZC:
	ts = Ts(op, &pc, wp);
	td = Td(op >> 6, &pc, wp);
	td.val = status_zero(td.val & ~ts);
	mem_w_Td(op >> 6, wp, td);
	NEXT;
S:
	ts = Ts(op, &pc, wp);
	td = Td(op >> 6, &pc, wp);
	//mem_w(td.addr, sub(td.val, ts));
	td.val = sub(td.val, ts);
	mem_w_Td(op >> 6, wp, td);
	NEXT;
C:
	ts = Ts(op, &pc, wp);
	td = Td(op >> 6, &pc, wp);
	status_arith(ts, td.val);
	Td_post_increment(op >> 6, wp, td, 2);
	NEXT;
A:
	ts = Ts(op, &pc, wp);
	td = Td(op >> 6, &pc, wp);
	td.val = add(td.val, ts);
	mem_w_Td(op >> 6, wp, td);
	NEXT;
MOV:
	ts = Ts(op, &pc, wp);
	td = Td(op >> 6, &pc, wp);
	td.val = status_zero(ts);
	mem_w_Td(op >> 6, wp, td);
	NEXT;
SOC:
	ts = Ts(op, &pc, wp);
	td = Td(op >> 6, &pc, wp);
	td.val = status_zero(td.val | ts);
	mem_w_Td(op >> 6, wp, td);
	NEXT;

SZCB:
	ts = TsB(op, &pc, wp);
	td = TdB(op >> 6, &pc, wp);
	if (td.addr & 1) {
		td.val &= ~(ts >> 8);
		status_parity(status_zero(td.val << 8));
	} else {
		td.val &= ~(ts & 0xff00);
		status_parity(status_zero(td.val & 0xff00));
	}
	mem_w_TdB(op >> 6, wp, td);
	NEXT;
SB:
	ts = TsB(op, &pc, wp);
	td = TdB(op >> 6, &pc, wp);
	if (td.addr & 1) {
		td.val = (td.val & 0xff00) | status_parity(sub(td.val << 8, ts) >> 8);
	} else {
		td.val = (td.val & 0x00ff) | status_parity(sub(td.val & 0xff00, ts));
	}
	mem_w_TdB(op >> 6, wp, td);
	NEXT;
CB:
	ts = TsB(op, &pc, wp);
	td = TdB(op >> 6, &pc, wp);
	if (td.addr & 1) {
		status_arith(status_parity(ts), td.val << 8);
	} else {
		status_arith(status_parity(ts), td.val & 0xff00);
	}
	NEXT;
AB:
	ts = TsB(op, &pc, wp);
	td = TdB(op >> 6, &pc, wp);
	if (td.addr & 1) {
		td.val = (td.val & 0xff00) | status_parity(add(td.val << 8, ts) >> 8);
	} else {
		td.val = (td.val & 0x00ff) | status_parity(add(td.val & 0xff00, ts));
	}
	mem_w_TdB(op >> 6, wp, td);
	NEXT;
MOVB:
	ts = TsB(op, &pc, wp);
	td = TdB(op >> 6, &pc, wp);
	if (td.addr & 1) {
		td.val = (td.val & 0xff00) | status_parity(status_zero(ts) >> 8);
	} else {
		td.val = (td.val & 0x00ff) | status_parity(status_zero(ts));
	}
	mem_w_TdB(op >> 6, wp, td);
	NEXT;

SOCB:
	ts = TsB(op, &pc, wp);
	td = TdB(op >> 6, &pc, wp);
	if (td.addr & 1) {
		td.val |= (ts >> 8);
		status_parity(status_zero(td.val << 8));
	} else {
		td.val |= ts;
		status_parity(status_zero(td.val & 0xff00));
	}
	mem_w_TdB(op >> 6, wp, td);
	NEXT;

COC:
	ts = Ts(op, &pc, wp);
	td.val = reg_r(wp, (op >> 6) & 15);
	if ((ts & td.val) == ts) set_EQ(); else clr_EQ();
	NEXT;
CZC:
	ts = Ts(op, &pc, wp);
	td.val = reg_r(wp, (op >> 6) & 15);
	if (!(ts & td.val)) set_EQ(); else clr_EQ();
	NEXT;
XOR:
	ts = Ts(op, &pc, wp);
	td.val = reg_r(wp, (op >> 6) & 15);
	reg_w(wp, (op >> 6) & 15, status_zero(ts ^ td.val));
	NEXT;

LDCR: {
	u8 idx, c = ((op >> 6) & 15) ?: 16;
	reg = (reg_r(wp, 12) & 0x1ffe) >> 1;
	if (c <= 8) {
		ts = TsB(op, &pc, wp) >> 8;
		status_parity(ts);
	} else {
		ts = Ts(op, &pc, wp);
	}
	//printf("LDCR c=%d reg=%d ts=%x\n", c, reg, ts);
	for (idx = 0; idx < c; idx++)
		cru_w(reg + idx, (1 << idx) & ts ? 1 : 0);
	status_zero(ts);
	NEXT;
}
STCR: {
	u8 idx, c = ((op >> 6) & 15) ?: 16;
	reg = (reg_r(wp, 12) & 0x1ffe) >> 1;
	if (c <= 8) {
		td = TdB(op, &pc, wp);
		td.val &= (td.addr & 1) ? 0xff00 : 0x00ff;
		u16 base = (td.addr & 1) ? 1 : 0x100;
		for (idx = 0; idx < c; idx++) {
			if (cru_r(reg + idx))
				td.val |= (base << idx);
		}
		mem_w_TdB(op, wp, td);
		//if (reg == 3 /*&& (va.val&0xff00) != 0xff00*/) {
		//	printf("STCR %d reg=%d val=%04x pc=%x kb=%d R4=%04x\n", c, reg, va.val, pc, keyboard_row,
		//			fast_ram[(wp-0x8300)/2+4]);
		//	
		//}
		status_parity(status_zero(td.val & 0xff00));
	} else {
		td = Td(op, &pc, wp);
		td.val = 0;
		for (idx = 0; idx < c; idx++) {
			if (cru_r(reg + idx))
				td.val |= (1 << idx);
		}
		//printf("STCR %d reg=%d val=%04x pc=%x\n", c, reg, va.val, pc);
		mem_w_Td(op, wp, td);
		status_zero(td.val);
	}
	//printf("STCR reg=%d %x %d  %x\n", reg, va.addr, c, va.val);
	// TODO status bits
	NEXT;
}
MPY: {
	u32 val;
	val = Ts(op, &pc, wp);
	reg = (op >> 6) & 15;
	//debug_log("MPY %04X x %04X = %08X\n", 
	//	val, reg_r(wp, reg), val * reg_r(wp, reg));
	val *= reg_r(wp, reg);
	reg_w(wp, reg, val >> 16);
	reg_w(wp, reg+1, val & 0xffff);
	NEXT;
}
DIV: {
	u32 val;
	ts = Ts(op, &pc, wp);
	reg = (op >> 6) & 15;
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
	NEXT;
}

#if 1
JMP: cyc += 2; pc += 2 * (s8)(op & 0xff); NEXT;
JLT: if (tst_LT()) goto JMP; NEXT;
JLE: if (tst_LE()) goto JMP; NEXT;
JEQ: if (tst_EQ()) goto JMP; NEXT;
JHE: if (tst_HE()) goto JMP; NEXT;
JGT: if (tst_GT()) goto JMP; NEXT;
JNE: if (!tst_EQ()) goto JMP; NEXT;
JNC: if (!tst_C()) goto JMP; NEXT;
JOC: if (tst_C()) goto JMP; NEXT;
JNO: if (!tst_OV()) goto JMP; NEXT;
JL: if (tst_L()) goto JMP; NEXT;
JH: if (tst_H()) goto JMP; NEXT;
JOP: if (tst_OP()) goto JMP; NEXT;
#else
JMP: JLT: JLE: JEQ: JHE: JGT: JNE: JNC: JOC: JNO: JL: JH: JOP: {
	// using a table ought to reduce branchiness
	static const u16 mask[] = {0, ST_AGT|ST_EQ, ST_LGT|ST_EQ, ST_EQ, ST_LGT|ST_EQ, ST_AGT, ST_EQ, ST_C, ST_C, ST_OV, ST_LGT|ST_EQ, ST_LGT|ST_EQ, ST_OP};
	static const u16 chek[] = {0, 0,                   ST_EQ, ST_EQ, ST_LGT|ST_EQ, ST_AGT,     0,    0, ST_C,     0,            0, ST_LGT,       ST_OP};
	if ((st & mask[subidx]) == chek[subidx]) {
		cyc += 2;
		pc += 2 * (s8)(op & 0xff);
	}
	NEXT;
}

#endif
SBO: cru_w((op & 0xff) + ((reg_r(wp, 12)&0x1ffe) >> 1), 1); NEXT;
SBZ: cru_w((op & 0xff) + ((reg_r(wp, 12)&0x1ffe) >> 1), 0); NEXT;
TB: status_equal(cru_r((op & 0xff) + ((reg_r(wp, 12) & 0x1ffe) >> 1)), 0); NEXT;

// carry set if last bit shifted out = 1
// LGT set if result != 0
// AGT set if MSB=0 and result != 0
// EQ set if result = 0

SRA:
	reg = reg_r(wp, op & 15);
	ts = shift_count(op, wp);
	if (reg & (1 << (ts-1))) set_C(); else clr_C();
	reg_w(wp, op & 15, status_zero(((short)reg) >> ts));
	NEXT;
SRL:
	reg = reg_r(wp, op & 15);
	ts = shift_count(op, wp);
	if (reg & (1 << (ts-1))) set_C(); else clr_C();
	reg_w(wp, op & 15, status_zero(reg >> ts));
	NEXT;
SLA:
	reg = reg_r(wp, op & 15);
	ts = shift_count(op, wp);
	if (reg & (0x8000 >> (ts-1))) set_C(); else clr_C();
	reg_w(wp, op & 15, status_zero(reg << ts));
	// overflow if MSB changes during shift
	if (ts == 16) {
	  if (reg) set_OV(); else clr_OV();
	} else {
	  u16 ov_mask = 0xffff << (15 - ts);
	  reg &= ov_mask;
	  if (reg != 0 && reg != ov_mask) set_OV(); else clr_OV();
	}
	NEXT;
SRC:
	reg = reg_r(wp, op & 15);
	ts = shift_count(op, wp);
	if (reg & (1 << (ts-1))) set_C(); else clr_C();
	reg_w(wp, op & 15, status_zero((reg << (16-ts))|(reg >> ts)));
	NEXT;

XOP:
	td = Td(op, &pc, wp); // source
	reg = (op >> 6) & 15; // XOP number usually 1 or 2
	ts = mem_r(0x0040 + (reg << 2)); // new WP

	mem_w(ts + 2*11, td.addr); // gAS copied to R11
	Td_post_increment(op, wp, td, 2);
	mem_w(ts + 2*13, wp); // WP to R13
	mem_w(ts + 2*14, pc); // PC to R14
	mem_w(ts + 2*15, get_st()); // ST to R15
	pc = mem_r(0x0042 + (reg << 2));
	wp = ts;
	set_X();
	//printf("XOP %04x %d  PC=%04x WP=%04x\n", td.val, reg, pc, wp);

	goto decode_op_now; // next instruction cannot be interrupted
BLWP:
	td = Td(op, &pc, wp);
	//debug_log("OLD pc=%04X wp=%04X st=%04X  NEW pc=%04X wp=%04X\n", pc, wp, st, safe_r(va.addr+2), va.val);
	mem_w(td.val + 2*13, wp);
	mem_w(td.val + 2*14, pc);
	mem_w(td.val + 2*15, get_st());
	pc = mem_r(td.addr + 2);
	wp = td.val;
	Td_post_increment(op, wp, td, 2);

	goto decode_op_now; // next instruction cannot be interrupted
	//NEXT;

B:    td = Td(op, &pc, wp); Td_post_increment(op, wp, td, 2); pc = td.addr; NEXT;
X:    td = Td(op, &pc, wp); Td_post_increment(op, wp, td, 2); op = td.val; /*printf("X op=%x pc=%x\n", op, pc);*/ goto execute_op;
CLR:  td = Td(op, &pc, wp); td.val = 0; mem_w_Td(op, wp, td); NEXT;
NEG:  td = Td(op, &pc, wp); td.val = sub(0, td.val); mem_w_Td(op, wp, td); NEXT;
INV:  td = Td(op, &pc, wp); td.val = status_zero(~td.val); mem_w_Td(op, wp, td); NEXT;
INC:  td = Td(op, &pc, wp); td.val = add(td.val,1); mem_w_Td(op, wp, td); NEXT;
INCT: td = Td(op, &pc, wp); td.val = add(td.val,2); mem_w_Td(op, wp, td); NEXT;
DEC:  td = Td(op, &pc, wp); td.val = sub(td.val,1); mem_w_Td(op, wp, td); NEXT;
DECT: td = Td(op, &pc, wp); td.val = sub(td.val,2); mem_w_Td(op, wp, td); NEXT;
BL:   td = Td(op, &pc, wp); Td_post_increment(op, wp, td, 2); reg_w(wp, 11, pc); pc = td.addr; NEXT;
SWPB: td = Td(op, &pc, wp); td.val = swpb(td.val); mem_w_Td(op, wp, td); NEXT;
SETO: td = Td(op, &pc, wp); td.val = 0xffff; mem_w_Td(op, wp, td); NEXT;
ABS:  td = Td(op, &pc, wp); status_zero(td.val);
  //st &= ~(ST_OV | ST_C); 
  clr_OV();
  clr_C(); // carry is not listed affected in manual, but it is
  if (td.val & 0x8000) {
      cyc += 2;
      if (td.val == 0x8000)
          set_OV();
      else {
          td.val = -td.val;
      }
  }
  mem_w_Td(op, wp, td);
  NEXT;

LI: reg_w(wp, op & 15, (reg_r(wp, op & 15), status_zero(mem_r(pc)))); pc += 2; NEXT;
AI: reg_w(wp, op & 15, add(reg_r(wp, op & 15), mem_r(pc))); pc += 2; NEXT;
ANDI: reg_w(wp, op & 15, status_zero(reg_r(wp, op & 15) & mem_r(pc))); pc += 2; NEXT;
ORI: reg_w(wp, op & 15, status_zero(reg_r(wp, op & 15) | mem_r(pc))); pc += 2; NEXT;
CI: status_arith(reg_r(wp, op & 15), mem_r(pc)); pc += 2; NEXT;
STWP: reg_w(wp, op & 15, wp); NEXT;
STST: reg_w(wp, op & 15, get_st()); NEXT;
LWPI: wp = mem_r(pc); pc += 2; NEXT;
LIMI: set_IM(mem_r(pc) & 15); pc += 2; gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; NEXT;
IDLE: debug_log("IDLE not implemented\n");/* TODO */ NEXT;
RSET: debug_log("RSET not implemented\n"); /* TODO */ NEXT;
RTWP: set_st(reg_r(wp, 15)); pc = reg_r(wp, 14); wp = reg_r(wp, 13); gPC=pc; gWP=wp; check_interrupt_level(); pc=gPC; wp=gWP; NEXT;
CKON: debug_log("CKON not implemented\n");/* TODO */ NEXT;
CKOF: debug_log("CKOF not implemented\n");/* TODO */ NEXT;
LREX: debug_log("LREX not implemented\n");/* TODO */ NEXT;
}

}
#endif

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


char disbuf[64] = {};
static u16 disasm_Ts(u16 pc, u16 op, int opsize)
{
	u16 i = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		print_asm("R%d", i);
		sprintf(disbuf+strlen(disbuf), " R%d=%04X", i, safe_r(gWP+i*2));
		break;
	case 1: // *Rx
		print_asm("*R%d", i);
		sprintf(disbuf+strlen(disbuf), " *R%d=%04X", i, safe_r(safe_r(gWP+i*2)));
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		pc += 2;
		print_asm("@>%04X", safe_r(pc));
		if (i) {
			print_asm("(R%d)", i);
			sprintf(disbuf+strlen(disbuf), " @>%04X(R%d=%X)=%04X", safe_r(pc), i, safe_r(gWP+i*2), safe_r(safe_r(pc)+safe_r(gWP+i*2)));
		} else {
			sprintf(disbuf+strlen(disbuf), " @>%04X=%04X", safe_r(pc), safe_r(safe_r(pc)));
		}
		break;
	case 3: // *Rx+
		print_asm("*R%d+", i);
		sprintf(disbuf+strlen(disbuf), " *R%d=%04X", i, safe_r(safe_r(gWP+(op&15)*2)-opsize));
		break;
	}
	return pc;
}

static u16 disasm_Bs(u16 pc, u16 op, int opsize)
{
	u16 i = op & 15;
	switch ((op >> 4) & 3) {
	case 0: // Rx
		print_asm("R%d", i);
		sprintf(disbuf+strlen(disbuf), " R%d=%04X", i, safe_r(gWP+i*2));
		break;
	case 1: // *Rx
		print_asm("*R%d", i);
		sprintf(disbuf+strlen(disbuf), " R%d=%04X", i, safe_r(gWP+i*2));
		break;
	case 2: // @yyyy(Rx) or @yyyy if Rx=0
		pc += 2;
		print_asm("@>%04X", safe_r(pc));
		if (i) {
			print_asm("(R%d)", i);
			sprintf(disbuf+strlen(disbuf), " @>%04X(R%d=%X)=%04X", safe_r(pc), i, safe_r(gWP+i*2), safe_r(safe_r(pc)+safe_r(gWP+i*2)));
		} else {
			sprintf(disbuf+strlen(disbuf), " @>%04X=%04X", safe_r(pc), safe_r(safe_r(pc)));
		}
		break;
	case 3: // *Rx+
		print_asm("*R%d+", i);
		sprintf(disbuf+strlen(disbuf), " *R%d=%04X", i, safe_r(safe_r(gWP+(op&15)*2)-opsize));
		break;
	}
	return pc;
}

// returns number of bytes in the disassembled instruction (2, 4, or 6)
int disasm(u16 pc)
{
	static const int jt1[]={L8(C,CB,A,AB,MOV,MOVB,SOC,SOCB)};
	static const int jt2[]={L4(SZC,SZCB,S,SB)};
	static const int jt3[]={L8(COC,CZC,XOR,XOP,LDCR,STCR,MPY,DIV)};
	static const int jt4[]={L8(JMP,JLT,JLE,JEQ,JHE,JGT,JNE,JNC),L8(JOC,JNO,JL,JH,JOP,SBO,SBZ,TB)};
	static const int jt5[]={L4(SRA,SRL,SLA,SRC)};
	static const int jt6[]={L8(BLWP,B,X,CLR,NEG,INV,INC,INCT),L8(DEC,DECT,BL,SWPB,SETO,ABS,BAD,BAD)};
	static const int jt7[]={L8(LI,AI,ANDI,ORI,CI,STWP,STST,LWPI),L8(LIMI,BAD,IDLE,RSET,RTWP,CKON,CKOF,LREX)};
	static const int * jt[] = {jt1, jt2, jt3, jt4, jt5, jt6, jt7};
	int save_cyc = cyc;
	u16 wp = gWP;
	u16 op = safe_r(pc), ts, td;
	u8 idx = __builtin_clz(op)-16;
	u16 start_pc = pc;

	disbuf[0] = 0; // clear register value string
	print_asm("%04X  %04X  ", pc, op);
	if (idx >= ARRAY_SIZE(jt)) {
		BAD:
		print_asm("DATA >%04X\n", op);
		cyc = save_cyc;
		return 2;
	}
	u8 subidx = (op >> decode[idx].shift) & decode[idx].mask;

	if (!names[idx][subidx])
		goto BAD;
	print_asm("%-5s", names[idx][subidx]);
	goto *(&&C + jt[idx][subidx]);

C: A: MOV: SOC: SZC: S:
	pc = disasm_Ts(pc, op, 2);
	print_asm(",");
	pc = disasm_Ts(pc, op >> 6, 2);
	goto done;

CB: AB: MOVB: SOCB: SZCB: SB:
	pc = disasm_Ts(pc, op, 1);
	print_asm(",");
	pc = disasm_Ts(pc, op >> 6, 1);
	goto done;
COC: CZC: XOR: MPY: DIV:
	pc = disasm_Ts(pc, op, 2);
	print_asm(",R%d", (op >> 6) & 15);
	goto done;
XOP:
	pc = disasm_Ts(pc, op, 2);
	print_asm(",%d", (op >> 6) & 15);
	goto done;
LDCR: STCR:
	pc = disasm_Ts(pc, op, 2);
	print_asm(",%d", (op >> 6) & 15 ?: 16);
	goto done;

JMP: JLT: JLE: JEQ: JHE: JGT: JNE: JNC: JOC: JNO: JL: JH: JOP:
	print_asm(">%04X", pc + 2 + 2 * (s8)(op & 0xff));
	sprintf(disbuf, "ST=%s%s%s%s%s",
		tst_EQ() ? "EQ " : "",
		tst_GT() ? "A> " : "",
		tst_H()&&tst_HE() ? "L> " : "",
		tst_C() ? "C " : "",
		tst_OV() ? "OV " : "");
	goto done;
SBO: SBZ: TB:
	print_asm("%d", op & 0xff);
	goto done;
SRA: SRL: SLA: SRC:
	print_asm("R%d,", op & 15);
	print_asm((op & 0x00f0) ? "%d" : "R0", (op >> 4) & 15);
	goto done;
BLWP: B: BL:
	pc = disasm_Bs(pc, op, 2);
	goto done;
X: CLR: NEG: INV: INC: INCT: DEC: DECT: SWPB: SETO: ABS:
	pc = disasm_Ts(pc, op, 2);
	goto done;
LI: AI: ANDI: ORI: CI:
	sprintf(disbuf, "R%d=%04X",
		op & 15, safe_r(wp + (op & 15)*2));
	pc += 2;
	print_asm("R%d,>%04X", op & 15, safe_r(pc));
	goto done;
STWP: STST:
	print_asm("R%d", op & 15);
	goto done;
IDLE: RSET: RTWP: CKON: CKOF: LREX:
	goto done;
LWPI: LIMI:
	pc += 2;
	print_asm(">%04X", safe_r(pc));
	goto done;

done:
	print_asm("\n");
	int ret = pc - start_pc;
	while (start_pc != pc) {
		start_pc += 2;
		print_asm("%04X  %04X\n", start_pc, safe_r(start_pc));
	}
	print_asm("     %s\n", disbuf);
	cyc = save_cyc;
	return ret;
}


