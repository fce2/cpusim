## (yet another) 65816 simulator

In C89 to be as portable as possible.
No dependencies, no installations or configurations needed and no annoying licenses.
Passes Tom Harte test zip for 65816 cycle exact.

| Define | Effect |
|-------|--------|
| `DEBUG` | enable cpu65816_dump() and unhandled opcode warnings |
| `SINGLE_INST` | bare static globals, no cpu pointer param |
| `MEM_IO` | memory r/w callbacks for I/O-mapped hardware |
| `COUNT_CYCLES` | step() returns cycle count, else void; enables cpu65816_run() |
| `NO_IO_MAP` | all MEM_IO via callback, no io_map bitmap (saves 64KB!) |
| `RICOH5A22` | SNES Ricoh 5A22 (WIP, will never happen, i have no real 65816 here) |

Key differences from the 6502:

| Feature | 6502 | 65816 |
|---------|------|-------|
| Address space | 64 KB | 16 MB (24-bit) |
| Registers | A, X, Y (8-bit) | A(16), X, Y(8/16), D, PBR, DBR |
| Stack pointer | 8-bit ($0100-$01FF) | 16-bit (native), 8-bit (emulation) |
| Mode | — | Emulation (E=1) ↔ Native (E=0) |
| M/X flags | — | Select 8/16-bit accumulator and index width |
| Vectors | 3 (NMI/RST/IRQ) | 9 addresses across 2 modes (emu: COP/BRK+IRQ/NMI/RST, native: COP/BRK/ABT/NMI/IRQ) |
| Block moves | — | MVN/MVP (up to 64KB per instruction) |
| Direct page | Zero page ($0000) | Relocatable via D register |
| BCD | 8-bit only | 8-bit and 16-bit |

### source files

#### the simulator itself

- `65816/cpu65816.h`
- `65816/cpu65816.c`
- `sim.h`

### testing

- `main001.cpp`
	just small things: NOP, ADD, LINK, Fibonacci

#### the big tests

Tom Harte (https://github.com/SingleStepTests/65816) did a great job again: a lot of tests for each opcode helped a lot !
