CFLAGS+=-O2 -g

bulwip: bulwip.o cpu.o sdl.o
bulwip:LDLIBS += $(shell pkg-config --libs sdl2)

bench: bulwip.c cpu.c sdl.c player.h
	gcc -O3 -DTEST bulwip.c cpu.c -o bench 

sdl.o: sdl.c player.h
sdl.o:CFLAGS += $(shell pkg-config --cflags sdl2)


994arom.h: 994arom.bin
	xxd -i -n rom994a $^ $@

994agrom.h: 994agrom.bin
	xxd -i -n grom994a $^ $@

99test2.bin_0000: 99test2.txt
	../xdt99-master/xas99.py -R -b 99test2.txt -o 99test2.bin

cputestc.bin: test/99test3.asm test/99test3.dat
	../xdt99/xas99.py -R -b test/99test3.asm -L test/99test3.lst -o cputestc.bin

eptest.bin_0000: eptest.asm
	../xdt99-master/xas99.py -R -b eptest.asm -o eptest.bin

test:
	dd if=cpudump.bin bs=1k skip=40 count=7|hexdump -C > cpudump.txt
	dd if=MEMDUMP.BIN bs=1 skip=40960 count=7294|tee 99test3.dat | hexdump -C > memdump.txt
