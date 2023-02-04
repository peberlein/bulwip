# BuLWiP - a TI/99 4/A emulator in C w/SDL2

- Features 9918A video and 9919 sound chip emulation.
- Lower memory usage, less lookup tables, keep important data in cache
- Instruction opcode decoding using CLZ instead of a lookup table
- Memory is designated as 16-bit words as seen from the CPU; no need for builtin_bswap16
- Memory accesses use a function pointer per 256 bytes
- Instruction loop is in a single function; keeping PC, WP, (maybe ST) in host CPU registers
- Instruction decoding functions inlined; better branch prediction

Requires ROM and GROM files: 994arom.bin, 994agrom.bin.  Put them in the same directory as the emulator executable.

Keyboard usage:
- ESC: Load Cartridges/Settings/Quit menu
- Ctrl-Home: Toggle debugger interface
- F5: Toggle CRT filter
- F11: Toggle full-screen
- Ctrl-F12: Reset and reload current cartridge/listings
- Arrow keys and Tab are mapped to Joystick 1.

Loading Cartridges:
- Files ending in G.BIN are assumed to be GROM, otherwise ROM.
- ROM files must be non-inverted (first bank is 0) format.
- Listing file is loaded automatically and must be named the same as the ROM with a .LST extension.

While debugger is open:
- F1: Run/Stop
- F2: Single instruction step
- Ctrl-F2: Single frame step
- Up/Down/PgUp/PgDn: move highlighted line in listing
- B: Toggle breakpoint at current line
- TODO Ctrl->B: Go to referenced label
- TODO Ctrl->F: Find text string
- TODO Ctrl->G: Repeat last find
- TODO Shift-Ctrl->G: Repeat last find, reverse direction
- TODO Home/End: Go to start/end of listing


