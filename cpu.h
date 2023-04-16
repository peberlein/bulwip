#ifndef CPU_H_
#define CPU_H_

typedef unsigned int u32;
typedef unsigned short u16;
typedef signed short s16;
typedef unsigned char u8;
typedef signed char s8;

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x)/sizeof(x[0]))
#endif

#ifndef CPU_TEST
#define ENABLE_UNDO
#define LOG_DISASM
#endif

#ifdef TEST
// use compiled roms to avoid I/O
#define COMPILED_ROMS
#define ENABLE_GIF
#else
#define USE_SDL
#endif

// none of these are used yet
//#define COMPILED_ROMS
//#define TRACE_GROM
//#define TRACE_VDP
//#define TRACE_CPU


// cpu.c
extern void cpu_reset(void);
extern void emu(void);
extern void single_step(void);
extern int disasm(u16 pc, int cycles);
extern char asm_text[256]; // output from disasm()
extern int disasm_cyc; // cpu.c
extern void interrupt(int level);

extern u16 get_pc(void);
extern u16 get_wp(void);
extern u16 get_st(void);
extern int add_cyc(int add);

//extern void cpu_break(int en);

extern void change_mapping(int base, int size, u16 *mem);
extern void set_mapping_safe(int base, int size,
	u16 (*read)(u16),
	u16 (*safe_read)(u16),
	void (*write)(u16, u16),
	u16 *mem);

extern void set_mapping(int base, int size,
	u16 (*read)(u16),
	void (*write)(u16, u16),
	u16 *mem);

// Safe reads attempt to avoid changing any state (but can add cpu cycles)
extern u16 safe_r(u16 address);
extern u16 map_r(u16 address);
extern void map_w(u16 address, u16 value);

extern void cpu_reset_breakpoints(void); // clear all
extern void cpu_set_breakpoint(u16 base, u16 size);




// sdl.h
enum {
	PAL_FPS = 50000,
	NTSC_FPS = 59940,
};
enum {
	FILTER_SMOOTH,
	FILTER_PIXELATED,
	FILTER_CRT,
};
extern void vdp_set_fps(int mfps /* fps*1000 */);
extern void snd_w(unsigned char byte);
extern void vdp_init(void);
extern void vdp_done(void);
extern int vdp_update(void);
//extern void vdp_line(unsigned int y, u8* restrict reg, u8* restrict ram);
extern void vdp_lock_texture(int line, int len, void**pixels, int *pitch);
extern void vdp_unlock_texture(void);
extern void vdp_text_window(const char *line, int w, int h, int x, int y, int highlight_line);
extern void vdp_text_pat(unsigned char *pat);
extern void mute(int en);
extern void vdp_window_scale(int scale);
extern void vdp_text_clear(int x, int y, int w, int h, unsigned int color);
extern void vdp_set_filter(void);

// ui.c
extern void load_listing(const char *filename, int bank);
extern int main_menu(void);
extern int debug_window(void);
extern void set_ui_key(int);



 // bulwip.c
extern void reset(void);
extern int debug_en; // debug mode active
enum {
	DEBUG_RUN = 0,
	DEBUG_STOP = 1,
	DEBUG_SINGLE_STEP = 2,
	DEBUG_FRAME_STEP = 3,
	DEBUG_SCANLINE_STEP = 4,
};
extern int debug_break; // 0=running 1=pause 2=single step 3=frame step
extern int debug_log(const char *fmt, ...);
extern int config_crt_filter; // 0=smooth 1=pixelated 2=crt

extern int breakpoint_read(u16 address); // called from brk_r()
extern int breakpoint_write(u16 address); // called from brk_w()
extern void set_breakpoint(u16 address, int bank, int enable); // enable=-1 to toggle, 0=disable, 1=enable, 2=paste
extern int get_breakpoint(int address, int bank); // returns enable or -1 if not found
extern void remove_breakpoint(u16 address, int bank);
extern int enum_breakpoint(int index, int *address, int *bank, int *enabled);
enum {
	BREAKPOINT_TOGGLE = -1,
	BREAKPOINT_DISABLE = 0,
	BREAKPOINT_ENABLE = 1,
	BREAKPOINT_PASTE = 2,
};

// external CRU functions
extern u8 cru_r(u16 bit);
extern void cru_w(u16 bit, u8 value);
extern void set_key(int k, int val); // called from vdp_update()

// things needed by ui.c
//extern int test_key(int key);
//extern int wait_key(void);
//extern void reset_keys(void);
extern void update_debug_window(void);
extern void redraw_vdp(void);
extern int vdp_update_or_menu(void);
extern void set_cart_name(char *name);
extern int get_cart_bank(void);
extern void paste_text(char *text, int old_fps);

/* more compact undo encoding:
  <PC_Words:2> 0=<PC:16> 1-3=go back words
  ST: status bits
  CYCLES: <count:8>
  
  Fast-RAM write: <addr:7> <value:16>
  Exp-RAM write: <addr:15> <value:16>
  Keyboard-line: <keyboard_row:3>
  
  VDP: decrement address (undo read)
  VDP: decrement address and set VDP RAM byte (undo write)
  VDP: address byte and set latch (undo addr write)
  VDP: address byte and clear latch (undo addr write)
  VDP: register byte and latch (undo register write)
  VDP: status byte (undo read status)
  VDP: decrement Y, wraps at zero
  
  GROM: address byte and latch
  GROM: decrement address (undo read)
  
  CB: old cart bank
  KB: old keyboard row
  
  
  
*/


// Each undo operation is 32-bits, encoded as:
enum {
	UNDO_PC = 0x0000, // 0x0000 <PC:16>   
	UNDO_WP = 0x0001, // 0x0001 <WP:16>
	UNDO_ST = 0x0002, // 0x0002 <ST:16>
	UNDO_CYC= 0x0003, // 0x0003 <Cyc:16>

	UNDO_VDPA = 0x0004, // 0x0004 <VdpAddr:16>
	UNDO_VDPD = 0x0005, // 0x0005 <VdpData:8>
	UNDO_VDPL = 0x0006, // 0x0006 <VdpLatch:1>
	UNDO_VDPST = 0x0007, // 0x0007 <VdpStatus:8>
	UNDO_VDPY = 0x0008, // 0x0008 <VdpY:16>
	UNDO_VDPR = 0x0009, // 0x0009 <VdpReg:8> <Value:8>

	UNDO_GA = 0x000a, // 0x000a <GromAddr:16>
	UNDO_GD = 0x000b, // 0x000b <GromData:8>
	UNDO_GL = 0x000c, // 0x000c <GromLatch:1>
	UNDO_CB = 0x000d, // 0x000d <CartBank:16>

	UNDO_KB = 0x000e, // 0x000e <KeyboardLine:3>

	UNDO_CPURAM = 0x0080, // 0x00 0b1 <FastRamAddr:7> <Value:16>
	UNDO_VDPRAM = 0x1000, // 0b0001   <VDPRamAddr:12> <Value:8>
		// 0b001
	UNDO_EXPRAM = 0x4000, // 0b01 <ExpRamAddr:14> <Value:16>
		// 0b1 
};

#ifdef ENABLE_UNDO
extern int undo_pop(void);
extern void undo_push(u16 op, unsigned int value);
extern void undo_fix_cyc(u16 value);
extern void undo_pcs(u16 *pcs, u8 *cycs, int count);
#else
// static definitions to turn into no-ops
static void undo_push(u16 op, unsigned int value) { }
#endif


//          col
//	case 3: //     = . , M N / fire1 fire2
//	case 4: // space L K J H ; left1 left2
//	case 5: // enter O I U Y P right1 right2
//	case 6: //       9 8 7 6 0 down1 down2
//	case 7: //  fctn 2 3 4 5 1 up1   up2
//	case 8: // shift S D F G A
//	case 9: //  ctrl W E R T Q
//	case 10:// menu  X C V B Z

enum {  // bits[5..3]=row  bits[2..0]=col
/*r0*/	TI_EQUALS, TI_SPACE, TI_ENTER, TI_FCTN=4, TI_SHIFT, TI_CTRL, TI_MENU,
/*r1*/	TI_PERIOD=8, TI_L, TI_O, TI_9, TI_2, TI_S, TI_W, TI_X,
/*r2*/	TI_COMMA,    TI_K, TI_I, TI_8, TI_3, TI_D, TI_E, TI_C,
/*r3*/	TI_M, TI_J, TI_U, TI_7, TI_4, TI_F, TI_R, TI_V,
/*r4*/	TI_N, TI_H, TI_Y, TI_6, TI_5, TI_G, TI_T, TI_B,
/*r5*/	TI_SLASH, TI_SEMICOLON, TI_P, TI_0, TI_1, TI_A, TI_Q, TI_Z,
/*r6*/	TI_FIRE1=48, TI_LEFT1, TI_RIGHT1, TI_DOWN1, TI_UP1,
/*r7*/	TI_FIRE2=56, TI_LEFT2, TI_RIGHT2, TI_DOWN2, TI_UP2,
	TI_ADDSHIFT = 1<<6,
	TI_ADDFCTN = 1<<7,
	TI_ADDCTRL = 1<<8,
	TI_ALPHALOCK = 1<<9,
	TI_PAGEUP = TI_ADDFCTN | TI_6, // 6=E/A Roll down  9=BACK
	TI_PAGEDN = TI_ADDFCTN | TI_4, // 4=E/A Roll up    6=PROC'D
	TI_DELETE = TI_ADDFCTN | TI_1,
	TI_INSERT = TI_ADDFCTN | TI_2,
	TI_HOME = 54,
	TI_END = 55,
};


// E/A strip
// DELch INSch DELln  ROLLup NXTscr ROLLdn TAB INSln ESCAPE

// DEL   INS   ERASE   CLEAR BEGIN PROC'D AID  REDO   BACK        QUIT
//  1!    2@     3#      4$    5%    6^    7&    8*    9(    0)    =+
//   Q     W:~    E:up    R:[   T:]   Y     U:_   I:?    P:"  O:'   /-
//    A:|   S:left D:right F:{   G:}   H     J     K      L    :;
//     Z:\   X:down C:`     V     B     N     M     ,<     .>

// Classic99 Function Key mapping, for future reference
// PC:  Up Down Left Right Tab F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 Ins Del PgDn PgUp Esc
// FCTN: E  X    S    D     7   1  2  3  4  5  6  7  8  9  0   2   1   4    6    9
// Arrows and Tab apply to joystick if CRU is scanning joysticks (inactive 3sec)

// Classic99 debug opcodes
#define C99_NORM 0x0110    // CPU Normal
#define C99_OVRD 0x0111    // CPU Overdrive
#define C99_SMAX 0x0112    // System Maximum
#define C99_BRK  0x0113    // Breakpoint
#define C99_QUIT 0x0114    // Quit emulator
#define C99_DBG  0x0120    // debug printf +register number


#endif
