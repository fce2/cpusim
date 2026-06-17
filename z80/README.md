## (yet another) Z80 simulator

**Same approach:**
	in C89 to be as portable as possible
	no dependencies, no installations, no annoying licenses

- Undocumented opcodes, MEMPTR/WZ, and Q flag

| Define | Effect |
|-------|--------|
| `SINGLE_INST` | bare static globals, no cpu pointer param (max speed) |
| `MEM_IO` | memory r/w callbacks for I/O-mapped hardware |
| `COUNT_CYCLES` | step() returns cycle count (uint8_t), else void |
| `NO_IO_MAP` | all MEM_IO via callback, skip io_map bitmap check |
| `NO_SZP_TABLE` | compute SZP flags at runtime, no 256B lookup |
| `DEBUG` | enable cpuZ80_dump() and illegal opcode warnings |

### source files

#### the simulator itself

- `z80/cpuZ80.h`
- `z80/cpuZ80.c`
- `sim.h`

### testing

#### tests

- `main001.cpp` — loads a tiny program, runs 8 steps, prints registers
  "hello world" for Z80: load A and B, ADD, store to memory, HALT

I could use help on testing.
I know 6502 quite well, not so much Z80.
