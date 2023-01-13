# BuLWiP - a TI/99 4/A emulator in C w/SDL

- Lower memory usage, less lookup tables, keep your important data in cache
- Instruction opcode decoding using CLZ instead of a giant lookup table
- Memory is designated as unsigned 16-bit words as seen from the CPU; no need for builtin_bswap16
- Memory accesses use a function pointer per 256 bytes
- Instruction loop is in a single function; keeping PC, WP, ST in host CPU registers
- Instruction decoding functions inlined; better branch prediction
