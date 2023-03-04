/*
 *  ui.c - TI 99/4A emulator
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
#define _GNU_SOURCE 
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif


#include "cpu.h"

static void *safe_alloc(size_t size)
{
	void *p = calloc(1, size);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return p;
}


static char* load_file(const char *filename, unsigned int *out_size)
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


unsigned int next_line(const char *lst, unsigned int len, unsigned int a)
{
	// skip non-newline chars until end of line
	while (a < len && lst[a] != '\r' && lst[a] != '\n')
		a++;
	// skip newline chars
	if ((lst[a] == '\r' && lst[a+1] == '\n') ||
	    (lst[a] == '\n' && lst[a+1] == '\r'))
		a++;
	return a < len ? a+1 : a;
}

unsigned int prev_line(const char *lst, unsigned int len, unsigned int a)
{
	// skip newline chars until end of prev line
	if (a > 0 && (lst[a-1] == '\r' || lst[a-1] == '\n')) {
		a--;
		if ((lst[a] == '\r' && lst[a-1] == '\n') ||
		    (lst[a] == '\n' && lst[a-1] == '\r'))
			a--;
	}
	// skip non-newline chars until start of line
	while (a > 0 && lst[a-1] != '\r' && lst[a-1] != '\n')
		a--;
	return a;
}

unsigned int step_lines(const char *lst, unsigned int len, unsigned int a, int delta)
{
	while (delta < 0) {
		a = prev_line(lst, len, a);
		delta++;
	}
	while (delta > 0) {
		a = next_line(lst, len, a);
		delta--;
	}
	return a;
}

static int count_lines(const char *lst, unsigned int len, unsigned int a)
{
	unsigned int b = 0;
	int ret = 0;

	while (a != b) {
		b = next_line(lst, len, b);
		ret++;
	}
	return ret;
}

static int line_len(const char *lst, unsigned int offset)
{
	int len = 0;
	while (lst[len] != 0 && lst[len] != '\r' && lst[len] != '\n') {
		len++;
	}
	return len;
}



int get_line_pc(const char *lst, unsigned int offset)
{
	unsigned int i = offset, n = 0;
	u16 pc = 0;
	// a line containing a PC may only have spaces or a line number preceding it
	while (i-offset < 5 && (lst[i] == ' ' || lst[i] == '\t')) i++; // skip any leading spaces

	if (i-offset == 4 && isxdigit(lst[i]) && isxdigit(lst[i+1]) && isxdigit(lst[i+2]) && isxdigit(lst[i+3]) && lst[i+4] == ':') {
		// gcc listings have addresses ending in colon "    6084:	72 aa 60 10 	sb @>6010(r10), r10"
	} else {
		while (i-offset < 5 && lst[i] >= '0' && lst[i] <= '9') i++; // skip the line number
		while (i-offset < 6 && (lst[i] == ' ' || lst[i] == '\t')) i++; // skip spaces between line number and pc
		if (i-offset < 5 || i-offset > 6) return -1; // PC must be at 5 or 6
	}
	for (n = 0; n < 4; n++) {
		char c = lst[i++];
		if (c >= '0' && c <= '9')
			pc = pc*16 + c - '0';
		else if (c >= 'A' && c <= 'F')
			pc = pc*16 + c - 'A' + 10;
		else if (c >= 'a' && c <= 'f')
			pc = pc*16 + c - 'a' + 10;
		else
			return -1;
	}
	return pc;
}


static struct list_segment {
	u16 start_addr, end_addr; // start address and end address
	int start_off, end_off; // start offset and end offset
	int bank; // use -1 for "BANK ALL"
	unsigned int src_len; // length of source code
	const char *src; // source code
	struct list_segment *next; // singly-linked list
} *listings = NULL;

static struct list_segment *new_list_segment(const char *src, unsigned int len)
{
	struct list_segment *seg, **list = &listings;
	while (*list != NULL) {
		list = &(*list)->next;
	}
	seg = safe_alloc(sizeof(*seg));
	if (seg) {
		*list = seg;
		seg->src = src;
		seg->src_len = len;
		seg->next = NULL;
	}
	return seg;
}

static void remove_conflicting_segments(struct list_segment *seg)
{
	struct list_segment *tmp, **list;
	for (list = &listings; *list; list = &(*list)->next) {
		tmp = *list;
		if (seg == tmp ||
		    seg->end_addr <= tmp->start_addr ||
		    seg->start_addr >= tmp->end_addr ||
		    (seg->bank != tmp->bank && seg->bank != -1 && tmp->bank != -1))
			continue;
		// overlapping segment, remove it
		printf("overlap %X..%X:%d and %X..%X:%d\n",
			seg->start_addr, seg->end_addr, seg->bank,
			tmp->start_addr, tmp->end_addr, tmp->bank);
		*list = tmp->next; // remove it
		free(tmp);
	}
}

static int case_compare_string(const char *a, const char *b)
{
	while (*b) {
		if (tolower(*a++) != tolower(*b++))
			return 1;
	}
	return 0;
}

static void end_segment(struct list_segment *seg, int pc, int off)
{
	seg->end_addr = pc != -1 ? pc : seg->start_addr;
	seg->end_off = off;
	remove_conflicting_segments(seg);
	printf("list %d-%d pc=%04x-%04x bank %d\n",
		seg->start_off, seg->end_off,
		seg->start_addr, seg->end_addr,
		seg->bank);
}


static void add_listing(const char *src, unsigned int len, int current_bank)
{
	//int current_bank = 0; // or -1 for all banks
	unsigned int a = 0;
	struct list_segment *seg = new_list_segment(src, len);
	int last_pc = -1;

	seg->start_off = a;
	seg->bank = current_bank;

	// process each line
	while (a < len) {
		int pc = get_line_pc(src, a);
		if (pc > 0) { // valid pc
			if (last_pc == -1)
				seg->start_addr = pc;
			if (pc > last_pc) {
				last_pc = pc;
			} else {
				// PC went backward? new segment
				end_segment(seg, last_pc, a);
				seg = new_list_segment(src, len);
				seg->start_off = a;
				seg->bank = current_bank;
				seg->start_addr = pc;
				last_pc = pc;
			}
		} else if (case_compare_string(src+a, "Disassembly of section .text") == 0) {
			current_bank = -1;
			end_segment(seg, last_pc, a);
			seg = new_list_segment(src, len);
			seg->start_off = a;
			seg->bank = current_bank;
			last_pc = -1;
		} else if (case_compare_string(src+a, "Disassembly of section .bank") == 0) {
			current_bank = atoi(src+a+strlen("Disassembly of section .bank"));
			end_segment(seg, last_pc, a);
			seg = new_list_segment(src, len);
			seg->start_off = a;
			seg->bank = current_bank;
			last_pc = -1;
		} else { // check for "BANK n or ALL"
			unsigned int i = a;
			while (i-a < 5 && (src[i] == ' ' || src[i] == '\t')) i++; // skip any leading spaces
			while (i-a < 5 && src[i] >= '0' && src[i] <= '9') i++; // skip the line number
			while ((src[i] == ' ' || src[i] == '\t')) i++; // skip spaces between line number and pc
			if (case_compare_string(src+i, "bank ") == 0) {
				i += 4;
				while (src[i] == ' ') i++; // skip any spaces after BANK
				if (case_compare_string(src+i, "all") == 0) {
					current_bank = -1;
				} else {
					current_bank = atoi(src+i);
				}
				if (seg->bank != current_bank) {
					// end the current segment and start a new one
					if (last_pc != -1) { // only if it had valid PC
						end_segment(seg, last_pc, a);
						seg = new_list_segment(src, len);
					}
					seg->start_off = a;
					seg->bank = current_bank;
					last_pc = -1;
				}
			}
		}
		a = next_line(src, len, a);
	}
	end_segment(seg, last_pc, a);
}

void load_listing(const char *filename, int bank)
{
	unsigned int romsrc_len = 0;
	char *romsrc = load_file(filename, &romsrc_len);
	if (romsrc)
		add_listing(romsrc, romsrc_len, bank);
}



// search the listing for the address pc
static struct list_segment* listing_search(u16 pc, unsigned int *offset)
{
	unsigned int a, b, c;
	int a_pc = -1, b_pc = -1, c_pc = -1;
	struct list_segment *seg = listings;
	const char *lst;
	unsigned int len;

	while (1) {
		if (!seg) return NULL;
		if (seg->start_addr <= pc && pc <= seg->end_addr) {
			if (pc < 0x6000 || pc >= 0x8000)
				break; // not banked
			if (seg->bank == -1 || seg->bank == get_cart_bank()) {
				break; // matched bank
			}
		}
		seg = seg->next;
	}
	lst = seg->src;
	len = seg->src_len;
	a = seg->start_off;
	b = seg->end_off;

	// we're treating negative or zero PC as invalid (listing may have 0000 for EQU PC)
	while (a_pc <= 0) {
		if (a >= seg->end_off) return 0;
		a_pc = get_line_pc(lst, a);
		//printf("a=%d pc=%04X\n", a, a_pc);
		if (a_pc > 0) break;
		a = next_line(lst, len, a);
	}
	while (b_pc <= 0) {
		if (b <= seg->start_off) return 0;
		b_pc = get_line_pc(lst, b);
		//printf("b=%d pc=%04X\n", b, b_pc);
		if (b_pc > 0) break;
		b = prev_line(lst, len, b);
	}
	//printf("a=%04X b=%04X c=%04X\n", a_pc, b_pc, c_pc);
	while (1) {
		if (a_pc == pc) {
			*offset = a;
			return seg;
		}
		if (b_pc == pc) {
			*offset = b;
			return seg;
		}
		if (a == b) {
			//printf("a=%04X b=%04x\n", a_pc, b_pc);
			return NULL;
		}
		c = (a+b)/2; // FIXME: could overflow
		c_pc = -1;
		while (c_pc <= 0) {
			c = prev_line(lst, len, c);
			if (c == 0) return NULL;
			c_pc = get_line_pc(lst, c);
		}
		if (c_pc == pc) {
			*offset = c;
			return seg;
		}
		//printf("a=%04X b=%04X c=%04X\n", a_pc, b_pc, c_pc);
		if (c_pc < pc) {
			if (a == c) {
				while (c_pc < pc) {
					c = next_line(lst, len, c);
					if (c >= len) return NULL;
					c_pc = get_line_pc(lst, c);
				}
				*offset = c;
				return seg;
			}
			a = c;
			a_pc = c_pc;
		} else {
			b = c;
			b_pc = c_pc;
		}
	}
	return NULL;
}

static void draw_listing(struct list_segment *seg, unsigned int *offset, int *line, int delta)
{
	while (delta < 0) {
		unsigned int old_offset = *offset;
		*offset = prev_line(seg->src, seg->src_len, *offset);
		if (*offset == old_offset && *line > 0)
			(*line)--;
		delta++;
	}
	while (delta > 0) {
		if (*line >= 14)
			*offset = next_line(seg->src, seg->src_len, *offset);
		else
			(*line)++;
		delta--;
	}
	vdp_text_window(seg->src + *offset, 80, 30, 640-80*6, 240, *line);

	int i = -1, pc;
	char bp[32] = "";
	unsigned int a = step_lines(seg->src, seg->src_len, *offset, *line);
	//printf("line=%d offset=%u a=%u\n", *line, *offset, a);

	pc = get_line_pc(seg->src, a);
	if (pc != -1) {
		i = get_breakpoint(pc, seg->bank);
		if (i != -1) {
			sprintf(bp, " BREAKPOINT%s ", i ? "" : " DISABLED");
			i = 0; // highlight the breakpoint indicator
		}
	}
	vdp_text_window(bp, 30,1, 322,240-8, i);
}



#define MENU_X 96
#define MENU_Y 96
#define MENU_DIR_W 20
#define MENU_DIR_H 20
#define CLEAR  0x00000000
#define SHADOW 0x80000000

static int fps_menu(void)
{
	char menu[] =
		"====================\n"
		"= NTSC 59.94 FPS   =\n"
		"= PAL 50 FPS       =\n"
		"= 100 FPS          =\n"
		"= 200 FPS          =\n"
		"= 1000 FPS         =\n"
		"= SYSTEM MAXIMUM   =\n"
		"====================\n";
	static int sel = 1;
	int w = 20, h = 8;
	
	while (1) {
		vdp_text_clear(MENU_X+8,MENU_Y+8, w,h, SHADOW);
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, sel);

		switch (wait_key()) {
		case TI_MENU: vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR); return 0;
		case TI_UP1: if (sel > 1) sel--; break;
		case TI_DOWN1: if (sel < 6) sel++; break;
		case TI_ENTER: case TI_FIRE1: case TI_SPACE:
			switch (sel) {
			case 1: vdp_set_fps(NTSC_FPS); break;
			case 2: vdp_set_fps(PAL_FPS); break;
			case 3: vdp_set_fps(100 * 1000); break;
			case 4: vdp_set_fps(200 * 1000); break;
			case 5: vdp_set_fps(1000 * 1000); break;
			case 6: vdp_set_fps(0); break;
			}
			vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR);
			return 0;
		case -1: return -1;
		}
	}
	return 0;
}

static int scale_menu(void)
{
	char menu[] =
		"=====================\n"
		"= 1X                =\n"
		"= 2X                =\n"
		"= 3X                =\n"
		"= 4X                =\n"
		"= 5X                =\n"
		"=====================\n";
	static int sel = 2;
	int w = 21, h = 7;

	while (1) {
		vdp_text_clear(MENU_X+8,MENU_Y+8, w,h, SHADOW);	
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, sel);

		switch (wait_key()) {
		case TI_MENU: vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR); return 0;
		case TI_UP1: if (sel > 1) sel--; break;
		case TI_DOWN1: if (sel < 5) sel++; break;
		case TI_ENTER: case TI_FIRE1: case TI_SPACE:
			vdp_window_scale(sel);
			vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR);
			return 0;
		case -1: return -1;
		}
	}
}

static int crt_filter_menu(void)
{
	char menu[] =
		"=====================\n"
		"= SMOOTHED          =\n"
		"= PIXELATED         =\n"
		"= CRT               =\n"
		"=                   =\n"
		"= THX2 GITHUB.COM/  =\n"
		"= LMP88959/NTSC-CRT =\n"
		"=====================\n";
	int sel = config_crt_filter+1;
	int w = 21, h = 8;
	
	while (1) {
		vdp_text_clear(MENU_X+8,MENU_Y+8, w,h, SHADOW);
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, sel);

		switch (wait_key()) {
		case TI_MENU: vdp_text_clear(MENU_X,MENU_Y, w+2,h+1,CLEAR); return 0;
		case TI_UP1: if (sel > 1) sel--; break;
		case TI_DOWN1: if (sel < 3) sel++; break;
		case TI_ENTER: case TI_FIRE1: case TI_SPACE:
			vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR);
			config_crt_filter = sel-1;
			vdp_set_filter(); // reinits the screen texture
			return 0;
		case -1: return -1;
		}
	}
}

static int settings_menu(void)
{
	char menu[] =
		"====================\n"
		"= FRAME RATE       =\n"
		"= WINDOW SCALE     =\n"
		"= VIDEO FILTER     =\n"
		"====================\n";
	int sel = 1;
	int w = 20, h = 5;
	
	while (1) {
		vdp_text_clear(MENU_X+8,MENU_Y+8, w,h, SHADOW);
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, sel);

		switch (wait_key()) {
		case TI_MENU: vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR); return 0;
		case TI_UP1: if (sel > 1) sel--; break;
		case TI_DOWN1: if (sel < 3) sel++; break;
		case TI_ENTER: case TI_FIRE1: case TI_SPACE:
			vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR);
			switch (sel) {
			case 1: if (fps_menu() == -1) return -1; break;
			case 2: if (scale_menu() == -1) return -1; break;
			case 3: if (crt_filter_menu() == -1) return -1; break;
			}
			break;
		case -1: return -1;
		}
	}
	return 0;
}

void append_str(char** restrict dst, const char* restrict src)
{
	unsigned int len = *dst ? strlen(*dst) : 0;
	*dst = realloc(*dst, len + strlen(src) + 1);
	strcpy(*dst + len, src);
}


static void dir_scroll(char* restrict dir,
                unsigned int* restrict offset,
                unsigned int* restrict sel,
                int delta)
{
	unsigned int dir_len = strlen(dir);

	while (delta < 0) {
		delta++;
		if (*sel == 0)
			*offset = prev_line(dir, dir_len, *offset);
		else
			(*sel)--;
	}
        if (delta > 0) {
		unsigned int off = *offset;
		unsigned int i;
	        unsigned int sel_limit;

		for (i = 0; i <= MENU_DIR_H; i++) {
			unsigned int new_off = next_line(dir, dir_len, off);
			if (new_off == off)
				break;
			off = new_off;
		}
		sel_limit = i-1;
		off = strlen(dir);
		if (off > 0) {
			off--;
			for (i = 0; i <= MENU_DIR_H; i++) {
				off = prev_line(dir, dir_len, off);
			}
		} else {
			sel_limit = 0;
		}
		//printf("sel_limit=%d off=%d *offset=%d\n", sel_limit, off, *offset);
		while (delta > 0) {
			delta--;
			if (*sel >= sel_limit)
				break;
			if (*sel < MENU_DIR_H-1) {
				(*sel)++;
			} else if (*offset <= off) {
				*offset = next_line(dir, dir_len, *offset);
			} else {
				break;
			}
		}
        }
}


// copy nth line from text at offset, returns allocated memory
// note: trailing whitespace trimmed
static char* copy_line(char *text, unsigned int offset, unsigned int line)
{
	char *result;
	unsigned int len = strlen(text);
	if (len == 0) return NULL;
	while (line--) {
		offset = next_line(text, len, offset);
	}
	len = next_line(text, len, offset) - offset;
	while (isspace(text[offset+len-1])) len--;
	result = safe_alloc(len+1);
	memcpy(result, text + offset, len);
	result[len] = 0;
	return result;
}


#ifndef _WIN32
int alphacasesort(const struct dirent **a, const struct dirent **b)
{
	return strcasecmp((*a)->d_name, (*b)->d_name);
}
#endif


static int load_cart_menu(void)
{
	static int dir_saved = 0, file_saved = 0;
	unsigned int dir_sel = 0, file_sel = 0;
	unsigned int dir_off = 0, file_off = 0;
	char *dirs = NULL, *files = NULL;
	int ret = 0;
	int x = 6*6, y = 5*8, w = MENU_DIR_W, h = MENU_DIR_H;

	enum { DIR_SIDE, FILE_SIDE } side = FILE_SIDE;

	vdp_text_clear(x+8,y+8, w*2,h, SHADOW);
rescan:
	// reset text buffers
	dirs = realloc(dirs, 1); dirs[0] = 0;
	files = realloc(files, 1); files[0] = 0;

#ifdef _WIN32
	{
		struct _finddata_t data;
		intptr_t find;

		find = _findfirst("*", &data);
		if (find != (intptr_t)INVALID_HANDLE_VALUE) {
			do {
				if ((data.attrib & FILE_ATTRIBUTE_DIRECTORY) &&
		        	    strcmp(data.name, ".") != 0) {
					append_str(&dirs, "[");
					append_str(&dirs, data.name);
					append_str(&dirs, "]\n");
				}
			} while (_findnext(find, &data) == 0);
			_findclose(find);
		}

		find = _findfirst("*.bin", &data);
		if (find != (intptr_t)INVALID_HANDLE_VALUE) {
			do {
				if (!(data.attrib & FILE_ATTRIBUTE_DIRECTORY)) {
					append_str(&files, data.name);
					append_str(&files, "\n");
				}
			} while (_findnext(find, &data) == 0);
			_findclose(find);
		}
	}
#else
	{
		struct dirent **namelist;
		int i, n = scandir(".", &namelist, NULL, alphacasesort);
		if (n < 0) {
			perror("scandir");
			return 0;
		}

		for (i = 0; i < n; i++) {
			struct dirent *d = namelist[i];
			int len = strlen(d->d_name);
			if (d->d_type == DT_DIR && !(len == 1 && d->d_name[0] == '.')) {
				append_str(&dirs, "[");
				append_str(&dirs, d->d_name);
				append_str(&dirs, "]\n");
			} else if (d->d_type == DT_REG && len > 4 && 
				   strcasecmp(d->d_name+len-4, ".bin") == 0) {
				append_str(&files, d->d_name);
				append_str(&files, "\n");
			}
			free(d);
		}
		free(namelist);

	}
#endif
	file_off = 0;
	dir_off = 0;

	file_sel = 0;
	dir_sel = 0;
	if (dir_saved) {
		dir_scroll(dirs, &dir_off, &dir_sel, dir_saved);
		dir_saved = 0;
	}
	if (file_saved) {
		dir_scroll(files, &file_off, &file_sel, file_saved);
		file_saved = 0;
	}
	//printf("files=%d off=%d sel=%d\n", strlen(files), file_off, file_sel);

	while (1) {
		vdp_text_window(dirs+dir_off, MENU_DIR_W,MENU_DIR_H, x,y, side == DIR_SIDE ? dir_sel : -1);
		vdp_text_window(files+file_off, MENU_DIR_W,MENU_DIR_H, x+MENU_DIR_W*6,y, side == FILE_SIDE ? file_sel : -1);
		if (side == FILE_SIDE) {
			switch (wait_key()) {
			case TI_MENU: goto done;
			case TI_UP1: dir_scroll(files, &file_off, &file_sel, -1); break;
			case TI_DOWN1: dir_scroll(files, &file_off, &file_sel, 1); break;
			case TI_PAGEUP: dir_scroll(files, &file_off, &file_sel, -MENU_DIR_H); break;
			case TI_PAGEDN: dir_scroll(files, &file_off, &file_sel, MENU_DIR_H); break;
			case TI_LEFT1: side = DIR_SIDE; break;
			case TI_FIRE1: case TI_ENTER: case TI_SPACE:
				{	char *entry = copy_line(files, file_off, file_sel);
					if (!entry) break;
					set_cart_name(entry);
					//printf("%s\n", entry);
					reset();
					ret = 1;
					goto done;
				}
			case -1: ret = -1; goto done;
			}
		} else { // side == DIR_SIDE
			switch (wait_key()) {
			case TI_MENU: goto done;
			case TI_UP1: dir_scroll(dirs, &dir_off, &dir_sel, -1); break;
			case TI_DOWN1: dir_scroll(dirs, &dir_off, &dir_sel, 1); break;
			case TI_PAGEUP: dir_scroll(dirs, &dir_off, &dir_sel, -MENU_DIR_H); break;
			case TI_PAGEDN: dir_scroll(dirs, &dir_off, &dir_sel, MENU_DIR_H); break;
			case TI_RIGHT1: side = FILE_SIDE; break;
			case TI_FIRE1: case TI_ENTER: case TI_SPACE:
				{	char *entry = copy_line(dirs, dir_off, dir_sel);
					if (!entry) break;
					// remove side [ ] chars
					int len = strlen(entry);
					memmove(entry, entry+1, len-2);
					entry[len-2] = 0;
					//printf("%s\n", entry);
#ifdef _WIN32
					_chdir(entry);
#else
					if (chdir(entry) < 0)
						perror("chdir");
#endif
					free(entry);
					goto rescan;
				}
			case -1: ret = -1; goto done;
			}
		}
	}
done:
	// save directory and file index
	dir_saved = count_lines(dirs, strlen(dirs), dir_off) + dir_sel;
	file_saved = count_lines(files, strlen(files), file_off) + file_sel;

	free(dirs);
	free(files);
	vdp_text_clear(x,y, w*2+2,h+1, CLEAR);
	return ret;
}

int main_menu(void)
{
	extern int menu_active; // sdl.c
	char menu[] =
		"====================\n"
		"= LOAD CARTRIDGE   =\n"
		//TODO "= LOAD E/A5        =\n"
		"= SETTINGS         =\n"
		"= QUIT EMULATOR    =\n"
		"====================\n";
	int sel = 1;
	int ret = 0;
	int w = 20, h = 5;


	menu_active = 1;

	vdp_text_clear(0, 0, 320/6,240/8, CLEAR);
	while (ret == 0) {
		vdp_text_clear(MENU_X+8, MENU_Y+8, w,h, 0x80000000); // shadow
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, sel);

		switch (wait_key()) {
		case TI_MENU: vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR);
			menu_active = 0; return 0;
		case TI_UP1: if (sel > 1) sel--; break;
		case TI_DOWN1: if (sel < 3) sel++; break;
		case TI_ENTER: case TI_FIRE1: case TI_SPACE:
			vdp_text_clear(MENU_X,MENU_Y, w+2,h+1, CLEAR);
			switch (sel) {
			case 1: ret = load_cart_menu(); break; // cartridge
			case 2: ret = settings_menu(); break;
			case 3: ret = -1; break; // quit
			}
			break;
		case -1: ret = -1; break;
		}
	}
	menu_active = 0;
	return ret;
}


static int show_breakpoints(void)
{
	extern int menu_active; // sdl.c
	int ret = 0;
	int w = 20, h = 0;
	char *menu = NULL;
	int len = 0;
	int i;
	int address, bank, enabled;
	while (enum_breakpoint(i++, &address, &bank, &enabled)) {
		printf("len=%d address=%x bank=%d enabled=%d\n", len, address, bank, enabled);
		menu = realloc(menu, len + 20);
		if (bank != -1) {
			sprintf(menu+len, "%04X:%d%s\n", address, bank,
				enabled ? "" : "disabled");
		} else {
			sprintf(menu+len, "%04X%s\n", address,
				enabled ? "" : "disabled");
		}
		h++;
		len = strlen(menu);
	}
	
	menu_active = 1;

	vdp_text_clear(0, 0, 320/6,240/8, CLEAR);
	while (ret == 0) {
		int len = strlen(menu);
		int k;
		vdp_text_clear(MENU_X+8,MENU_Y+8, w,h, 0x80000000); // shadow
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, -1);
		vdp_text_window(menu, w-2,1, MENU_X+6,MENU_Y+8, -1);
		k = wait_key();
		if (k == -1) { ret = -1; break; }

	}
	menu_active = 0;
	return ret;

}




char *find_stack = NULL;  // array of string pointers, NULL terminated

static void string_stack_push(char **stack, char *text)
{
	char *p = *stack;
	int len = p ? strlen(p) : 0;
	int tlen = strlen(text);
	// check to see if item is already in it

	// otherwise add it to the stack

	// add space for newline and nul terminator
	p = realloc(p, len + tlen + 2);
	memmove(p + tlen + 1, p, len + 1);
	memcpy(p, text, tlen);
	p[tlen] = '\n';
	if (len == 0) p[len + tlen + 1] = 0;

	*stack = p;
}

static int text_entry(char *title, char **stack)
{
	extern int menu_active; // sdl.c
	int ret = 0;
	int w = 20, h = 3;
	char menu[] =
		"====================\n"
		"=                  =\n"
		"====================\n";
	char text[20] = {'_'};
	unsigned int offset = stack && *stack ? strlen(*stack) : 0;

	{
		int len = strlen(title);
		int x = (w - len) / 2;

		menu[x-1] = ' ';
		menu[x+len] = ' ';
		memcpy(menu+x, title, len);
	}

	menu_active = 1;

	vdp_text_clear(0, 0, 320/6,240/8, CLEAR);
	while (ret == 0) {
		int len = strlen(text);
		int k;
		vdp_text_clear(MENU_X+8,MENU_Y+8, w,h, 0x80000000); // shadow
		vdp_text_window(menu, w,h, MENU_X,MENU_Y, -1);
		vdp_text_window(text, w-2,1, MENU_X+6,MENU_Y+8, -1);
		k = wait_key();
		if (k == -1) { ret = -1; break; }
		if (k == TI_ENTER) {
			text[len-1] = 0; // erase underscore
			string_stack_push(stack, text);
			ret = 1;
			break;
		}
		if (k == TI_MENU) break;
		if (k == TI_S+TI_ADDFCTN || k == TI_LEFT1) {
			if (len > 1) {
				text[len-2] = '_';
				text[len-1] = 0;
			}
		} else if (k == TI_E+TI_ADDFCTN || k == TI_UP1) {
			if (*stack && offset > 0) {
				int len;
				offset = prev_line(*stack, strlen(*stack), offset);

				

			}
		} else if (k == TI_S+TI_ADDFCTN || k == TI_DOWN1) {
			if (*stack) {

			}
		} else if ((k&0x3f) == TI_CTRL || (k&0x3f) == TI_FCTN || (k&0x3f) == TI_SHIFT) {
			// do nothing
		} else if ((k&0x3f) <= TI_Z && ((k&TI_ADDCTRL) == 0) && len < sizeof(text)-2) {
			const char *table =
				k & TI_ADDFCTN ?
				"        "
				"  '   ~ "
				"  ?    `"
				"  _  {[ "
				"     }] "
				"  \"  | \\" :
				k & TI_ADDSHIFT ?
				"+       "
				">LO(@SWX"
				"<KI*#DEC"
				"MJU&$FRV"
				"NHY^%GTB"
				"-:P)!AQZ" :
				// unshifted
				"=       "
				".lo92swx"
				",ki83dec"
				"mju74frv"
				"nhy65gtb"
				"/;p01aqz";
			//printf("k=%d  table=%c\n", k&0x3f, table[k&0x3f]);
			text[len-1] = table[k&0x3f];
			text[len] = '_';
			text[len+1] = 0;
		} else {
			printf("%d %s%s%s \n", k&0x3f,
				k&TI_ADDFCTN?" FCTN":"",
				k&TI_ADDCTRL?" CTRL":"",
				k&TI_ADDSHIFT?" SHIFT":""
				);
		}


	}
	menu_active = 0;
	//printf("%s: text='%s' ret=%d\n", __func__, text, ret);
	return ret;
}

// returns 1 if text was found, otherwise 0
static int do_find(const char *lst, unsigned int *offset, int *line)
{
	if (find_stack == NULL) return 0;
	unsigned int lst_len = strlen(lst);
	int i = *line;
	unsigned int o = *offset;

	for (i = *line; i; i--) {
		o = next_line(lst, lst_len, o);
	}
	int word_len = line_len(find_stack, 0);
	char word[word_len];
	memcpy(word, find_stack, word_len);
	word[word_len] = 0;
	o = next_line(lst, lst_len, o);
	char *p = strstr(lst + o, word);
	printf("word=%s p=%p\n", word, p);
	if (!p) return 0;
	// found something
	o = p - lst;
	// don't go to prev line if found at start of line
	if (o > 0 && lst[o-1] != '\n' && lst[o-1] != '\r')
		*offset = prev_line(lst, lst_len, o);
	else
		*offset = o;
	return 1;
}

// returns 1 if text was found, otherwise 0
static int do_find_reverse(const char *lst, unsigned int *offset, int *line)
{
	if (find_stack == NULL) return 0;
	unsigned int lst_len = strlen(lst);
	int i = *line;
	unsigned int o = *offset;

	for (i = *line; i; i--) {
		o = next_line(lst, lst_len, o);
	}
	int word_len = line_len(find_stack, 0);
	char word[word_len];
	memcpy(word, find_stack, word_len);
	word[word_len] = 0;
	const char *p;
	for (p = lst + o; p >= lst; p--) {
		if (memcmp(p, word, word_len) == 0)
			break;
	}

	//printf("word=%s p=%p\n", word, p);
	if (p < lst) return 0;
	// found something
	o = p - lst;
	// don't go to prev line if found at start of line
	if (o > 0 && lst[o-1] != '\n' && lst[o-1] != '\r')
		*offset = prev_line(lst, lst_len, o);
	else
		*offset = o;
	return 1;
}

static void print_key(int k)
{
	const char *table =
		k & TI_ADDFCTN ?
		"        "
		"  '   ~ "
		"  ?    `"
		"  _  {[ "
		"     }] "
		"  \"  | \\" :
		k & TI_ADDSHIFT ?
		"+       "
		">LO(@SWX"
		"<KI*#DEC"
		"MJU&$FRV"
		"NHY^%GTB"
		"-:P)!AQZ" :
		// unshifted
		"=       "
		".lo92swx"
		",ki83dec"
		"mju74frv"
		"nhy65gtb"
		"/;p01aqz";

	if (k == -1) printf("quit\n");
	else {
		char tmp[] =
		"=       "
		".lo92swx"
		",ki83dec"
		"mju74frv"
		"nhy65gtb"
		"/;p01aqz";
		printf("%c%s%s%s (%c)\n",
			tmp[k&0x3f],
			k&TI_ADDFCTN?" FCTN":"",
			k&TI_ADDSHIFT?" SHIFT":"",
			k&TI_ADDCTRL?" CTRL":"",
			table[k&0x3f]);
	}
}


int debug_window(void)
{

debug_refresh:
	//printf("before debug %d\n", add_cyc(0));

	update_debug_window();
	//printf("after debug %d saved=%d\n", add_cyc(0), saved_cyc);

	unsigned int offset;
	struct list_segment *seg = listing_search(get_pc(), &offset);
	int line;
debug_redraw:
	line = 14;

	if (seg) {
		draw_listing(seg, &offset, &line, -line);
	}

	if (debug_break) {
		mute(1); // turn off sounds while stopped
	}
	while (debug_break != DEBUG_RUN) {
		if (debug_break == DEBUG_FRAME_STEP) {
			// frame step
			debug_break = DEBUG_STOP;
			return 0; // return to draw one whole frame, then come back here
		}
		if (debug_break == DEBUG_SINGLE_STEP) {
			int c = add_cyc(0); // get current cycle count
			if (c > 0)
				return 0; // VDP update needed

			single_step();
			debug_break = DEBUG_STOP;
			goto debug_refresh;
		}

		int k = wait_key();
		//print_key(k);
		if (k == -1) return -1; // quit
		if (k == TI_MENU) {
			if (main_menu() == -1) return -1; // quit
		}

		if (seg) {
			switch (k) {
			case TI_PAGEUP:  // PGUP / 9:BACK
				draw_listing(seg, &offset, &line, -14); break;
			case TI_PAGEDN:  // PGDN / 6:PROC'D
				draw_listing(seg, &offset, &line, 14); break;
			case TI_UP1: case TI_E+TI_ADDFCTN:
				draw_listing(seg, &offset, &line, -1); break;
			case TI_DOWN1: case TI_X+TI_ADDFCTN:
				draw_listing(seg, &offset, &line, 1); break;
			case TI_HOME:
				offset = 0; line = 0;
				draw_listing(seg, &offset, &line, 0);
				break;
			case TI_END: {
				unsigned int off;
				do {
					off = offset;
					offset = next_line(seg->src, seg->src_len, offset);
				} while (off != offset);
				goto debug_redraw;
			}
			case TI_F+TI_ADDCTRL: {
				int ret = text_entry("FIND", &find_stack);
				if (ret == -1) return -1;
				if (ret == 1) {
					if (do_find(seg->src, &offset, &line))
						goto debug_redraw;
				}
				break;
			}
			case TI_G+TI_ADDCTRL+TI_ADDSHIFT:
				if (do_find_reverse(seg->src, &offset, &line))
					goto debug_redraw;
				break;

			case TI_G+TI_ADDCTRL:
				if (do_find(seg->src, &offset, &line))
					goto debug_redraw;
				break;
			}
		}
		if (k == TI_B) {
			unsigned int i, off = offset;
			int pc;
			for (i = 0; i < line; i++)
				off = next_line(seg->src, seg->src_len, off);
			pc = get_line_pc(seg->src, off);
			if (pc != -1) {
				// breakpoint
				printf("breakpoint pc=%04X bank=%d\n", pc, seg->bank);
				set_breakpoint(pc, seg->bank, BREAKPOINT_TOGGLE);
				draw_listing(seg, &offset, &line, 0);
			}
		} else if (k == TI_5+TI_ADDFCTN) {
			// F5 list breakpoints
			if (show_breakpoints() == -1)
				return -1;
		}
	}
	mute(0);
	return 0;
}

