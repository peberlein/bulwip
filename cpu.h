#ifndef CPU_H_
#define CPU_H_

typedef unsigned short u16;
typedef signed short s16;
typedef unsigned char u8;
typedef signed char s8;

// cpu.c
extern void cpu_reset(void);
extern void emu(void);
extern int disasm(u16 pc);
extern void interrupt(int level);

extern u16 get_pc(void);
extern u16 get_wp(void);
extern u16 get_st(void);
extern int add_cyc(int add);
extern void cpu_break(int en);

extern void change_mapping(int base, int size, u16 *mem);
extern void set_mapping_safe(int base,
	u16 (*read)(u16),
	u16 (*safe_read)(u16),
	void (*write)(u16, u16),
	u16 *mem);

extern void set_mapping(int base,
	u16 (*read)(u16),
	void (*write)(u16, u16),
	u16 *mem);

extern u16 safe_r(u16 address);
extern u16 map_r(u16 address);
extern void map_w(u16 address, u16 value);

#if 0
// sdl.h
extern void snd_w(unsigned char byte);
extern void vdp_init(void);
extern void vdp_done(void);
extern int vdp_update(void);
extern void vdp_line(unsigned int y, u8* restrict reg, u8* restrict ram);
#endif

#endif
