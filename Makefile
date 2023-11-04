#CC=clang
CFLAGS+=-O3

#CRT=NTSC-CRT-v2/crt_ntsc.o NTSC-CRT-v2/crt_core.o
#CRT_H=NTSC-CRT-v2/crt_core.h NTSC-CRT-v2/crt_ntsc.h
CRT=NTSC-CRT/crt.o
CRT_H=NTSC-CRT/crt.h

bulwip: bulwip.o cpu.o ui.o sdl.o $(CRT)
bulwip:LDLIBS += $(shell pkg-config --libs sdl2)

bench: bulwip.c cpu.c sdl.c player.h cpu.h
	gcc -O3 -DTEST bulwip.c cpu.c -o bench

sdl.o: sdl.c player.h cpu.h $(CRT_H)
sdl.o:CFLAGS += $(shell pkg-config --cflags sdl2) -DENABLE_CRT

cpu.o: cpu.c cpu.h
bulwip.o: bulwip.c cpu.h
ui.o: ui.c cpu.h

NTSC-CRT-v2/crt_ntsc.o: NTSC-CRT-v2/crt_ntsc.c $(CRT_H)
NTSC-CRT-v2/crt_core.o: NTSC-CRT-v2/crt_core.c $(CRT_H)
NTSC-CRT/crt.o: NTSC-CRT/crt.c $(CRT_H)

994arom.h: 994arom.bin
	(echo 'const u16 rom994a[] = {'; \
	dd if=$^ conv=swab,sync ibs=8K | hexdump -v -e '8/2 "0x%04x," "\n"'; \
	echo '};') > $@

994agrom.h: 994agrom.bin
	(echo 'const u8 grom994a[] = {'; \
	hexdump -v -e '16/1 "0x%02x, " "\n"' $^; \
	echo '};') > $@

megademo.h: ../megademo8.bin
	(echo 'const u16 megademo[] = {'; \
	dd if=$^ conv=swab,sync ibs=8K | hexdump -v -e '8/2 "0x%04x," "\n"'; \
	echo '};') > $@
mbtest.h: test/mbtest.bin
	(echo 'const u16 mbtest[] = {'; \
	dd if=$^ conv=swab,sync ibs=8K | hexdump -v -e '8/2 "0x%04x," "\n"'; \
	echo '};') > $@



99test2.bin_0000: 99test2.txt
	../xdt99-master/xas99.py -R -b 99test2.txt -o 99test2.bin

cputestc.bin: test/99test3.asm test/99test3.dat
	../xdt99/xas99.py -R -b test/99test3.asm -L test/99test3.lst -o cputestc.bin

eptest.bin_0000: eptest.asm
	../xdt99-master/xas99.py -R -b eptest.asm -o eptest.bin

test:
	dd if=cpudump.bin bs=1k skip=40 count=7|hexdump -C > cpudump.txt
	dd if=MEMDUMP.BIN bs=1 skip=40960 count=7294|tee 99test3.dat | hexdump -C > memdump.txt

cputest: cpu.c cpu.h
	gcc -g cpu.c -DCPU_TEST -o $@
