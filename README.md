# BuLWiP - a TI/99 4/A emulator in C w/SDL2

- Features 9918A video and 9919 sound chip emulation.
- Lower memory usage, less lookup tables, keep important data in cache
- Instruction opcode decoding using CLZ instead of a lookup table
- Memory is designated as 16-bit words as seen from the CPU; no need for builtin_bswap16
- Memory accesses use a function pointer per 256 bytes
- Instruction loop is in a single function; keeping PC, WP, (maybe ST) in host CPU registers
- Instruction decoding functions inlined; better branch prediction

Requires ROM and GROM files: 994arom.bin, 994agrom.bin.  Put them in the same directory as the emulator executable. 
(If you want to have a ROM source listing, it should be named '994arom.lst'.)

Keyboard usage:
- ESC: Load Cartridges/Settings/Quit menu
- F11: Toggle full-screen
- F12 or Ctrl-Home: Toggle debugger interface
- Ctrl-F12: Reset and reload current cartridge/listings
- Shift-Insert: Paste from clipboard
- Arrow keys and Tab are mapped to Joystick 1.

Loading Cartridges:
- Files ending in G.BIN are assumed to be GROM, otherwise ROM.
- ROM files must be non-inverted (first bank is 0) format.
- Listing file is loaded automatically and must be named the same as the ROM with a .LST extension.
- Cartridge files may be loaded by drag-n-drop onto window.

While debugger is open:
- F1: Run/Stop
- F2: Single instruction step
- Ctrl-F2: Single frame step
- Up/Down/PgUp/PgDn: move highlighted line in listing
- Home/End: Go to start/end of listing
- Ctrl-F: Find text string
- Ctrl-G: Repeat last find
- Shift-Ctrl-G: Repeat last find, reverse direction
- B: Toggle breakpoint at current line (red=stop, green=go)
- Del: Remove breakpoint at current line
- F5: List breakpoints
  - Enter: Go to selected breakpoint
  - Space: Toggle selected breakpoint
  - Del: Remove selected breakpoint
- R: Register select, then Enter to jump to address
- Z: Reverse instruction step
- Shift-Z: Reverse instruction step until PC goes lower (good for rewinding out of a loop)
- TODO Ctrl->B: Go to referenced label


