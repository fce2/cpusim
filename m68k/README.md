## (yet another) 68000 simulator

In C89 to be as portable as possible.
No dependencies, no installations or configurations needed and no annoying licenses.
Passes Tom Harte test zip for 68000 cycle exact.

### source files

#### the simulator itself

- `m68k/cpu68000.h`
- `m68k/cpu68000.c`
- `sim.h`

### testing

- `main001.cpp`
	just small things: NOP, ADD, LINK, Fibonacci

#### the big tests

Tom Harte (https://github.com/SingleStepTests/680x0) did a great job again: a lot of tests for each opcode helped a lot !
