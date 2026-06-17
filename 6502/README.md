## (yet another) 6502 simulator

I'm working on this many decades now ;-)
Time to release before i die...

**Portable:**
	in C89 to be as portable as possible.

**Selfcontained:**
	no dependencies.
	no installations or configurations needed.
	and no annoying licenses.

- Full NMOS and CMOS support (5 variants)
- All bugs preserved (e.g. jmpbug)
- Cycle exact (if needed)

Optimized for various usages with many defines ("not too good" documented yet).

| Define | Effect |
|-------|--------|
| `DEBUG` | enable cpu6502_dump() and illegal opcode warnings |
| `SINGLE_INST` | bare static globals, no cpu pointer param |
| `MEM_IO` | memory r/w callbacks for I/O-mapped hardware |
| `COUNT_CYCLES` | step() returns cycle count, else void |
| `ILLEGAL` | implement NMOS illegal opcodes: KIL/JAM halts CPU |
| `NO_NZ_TABLE` | branch-based SETNZ instead of 256B lookup |
| `NO_IO_MAP` | all MEM_IO via callback, no io_map bitmap (saves 256B) |

CPU variants, default `MOS6502`:

| Variant | Description |
|---------|-------------|
| `MOS6502` | NMOS (JMP ind page-wrap bug, BCD flags mid-result, the "original") |
| `RICOH2A03` | NMOS, no BCD (decimal flag ignored, NES) |
| `ROCKWELL65C02` | CMOS (fixed JMP ind, BCD flags from final result) |
| `SYNERTEK65C02` | CMOS (RMW illegals → NOPs) |
| `WDC65C02` | CMOS + BBR/BBS/RMB/SMB, STP/WAI |

Thanks to all brothers and sisters in mind:
All 6502 simulator developers sharing their work, I found 20 projects without searching too much !
I couldn't test all of them because of missing dependencies or needed installations or configurations which failed.

### source files

#### the simulator itself

- `6502/cpu6502.h`
- `6502/cpu6502.c`
- `sim.h`

```
	#include "cpu6502.h"
	#include <stdio.h>
	#include <stdlib.h>
	#include <string.h>

	static void load(u8 *ram, const char* path, u16 addr) {
		FILE* f = fopen(path, "rb");
		if (!f) { fprintf(stderr, "Can't open %s\n", path); exit(1); }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		fread(&ram[addr], 1, (size_t)n, f);
		fclose(f);
	}

	int main() {
		static u8 ram[65536] = {};
		load(ram, "basic_a000.rom",  0xA000);
		load(ram, "kernal_e000.rom", 0xE000);
		CPU6502 cpu;
		cpu6502_init(&cpu, ram, NULL, NULL);
		cpu6502_reset(&cpu);
		if (cpu.PC != 0xFCE2) { printf("FAIL: wrong reset vector\n"); return 1; }
		int cycles = 0, i = 0;
		for (; i < 20; i++) {
			cpu6502_dump(&cpu);
			cycles += cpu6502_step(&cpu);
		}
		printf("%d steps (%d cycles): PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X\n", i, cycles, cpu.PC, cpu.A, cpu.X, cpu.Y, cpu.SP);
		return 0;
	}
```
gives
```
	PC=FCE2 A=00 X=00 Y=00 P=24:nvUbdIzc SP=FD
	PC=FCE4 A=00 X=FF Y=00 P=A4:NvUbdIzc SP=FD
	PC=FCE5 A=00 X=FF Y=00 P=A4:NvUbdIzc SP=FD
	PC=FCE6 A=00 X=FF Y=00 P=A4:NvUbdIzc SP=FF
	PC=FCE7 A=00 X=FF Y=00 P=A4:NvUbdIzc SP=FF
	PC=FD02 A=00 X=FF Y=00 P=A4:NvUbdIzc SP=FD
	PC=FD04 A=00 X=05 Y=00 P=24:nvUbdIzc SP=FD
	PC=FD07 A=30 X=05 Y=00 P=24:nvUbdIzc SP=FD
	PC=FD0A A=30 X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FD0F A=30 X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FCEA A=30 X=05 Y=00 P=25:nvUbdIzC SP=FF
	PC=FCEF A=30 X=05 Y=00 P=25:nvUbdIzC SP=FF
	PC=FCF2 A=30 X=05 Y=00 P=25:nvUbdIzC SP=FF
	PC=FDA3 A=30 X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FDA5 A=7F X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FDA8 A=7F X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FDAB A=7F X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FDAE A=7F X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FDB0 A=08 X=05 Y=00 P=25:nvUbdIzC SP=FD
	PC=FDB3 A=08 X=05 Y=00 P=25:nvUbdIzC SP=FD
	20 steps (70 cycles): PC=$FDB6 A=$08 X=$05 Y=$00 SP=$FD
```

### testing

#### some little tests

- `main.h` — some very little test helpers

- `main000.cpp` — 6502-on-6502
  probably the smallest 6502 simulator currently simulating itself: main000.prg is **5591 bytes** (without printf) !
  and maybe the only time in computing history a NULLPTR is a valid ptr: `#define ram ((u8*)0)` !

- `main001.cpp` — added some "useful" output to main000.cpp, even with printf still very small
  starts booting (on) a c64 ;-)

- `main002.cpp` — a bit more functional, not a single global instance anymore

- `main003.cpp` — added basic irqs, let the c64 boot to READY.
```
	2706955 cycles, 159 IRQs: PC=$E5CD A=$00
	Jiffy: $04 $00 $00

		**** COMMODORE 64 BASIC V2 ****

	 64K RAM SYSTEM  51216 BASIC BYTES FREE

	READY.
```

#### the big tests

Thanks to all cpu testers sharing results.
Tom Harte (https://github.com/SingleStepTests/65x02) did a great job: 10000 tests for each opcode helped a lot !
Klaus Dormann (https://github.com/Klaus2m5/6502_65C02_functional_tests) helped also a lot to get things right !
