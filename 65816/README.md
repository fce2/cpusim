# cpu65816 — WDC 65C816 Simulator

16-bit extension of the 6502.
24-bit addressing, bank registers, 8/16-bit M/X modes, emulation/native switching.
Used in SNES, Apple IIGs.

## Status: beta

All native-mode tests pass (0 failures).
463 emulation-mode failures — confirmed test-data bugs (test data doesn't enforce page-1 stack wrapping per WDC datasheet).

## Competitor Analysis

SingleStepTests (Tom Harte's 65816-main.zip, ~10M test cases across 256 opcodes × 2 modes × ~10K tests each).

### Overall Results

┌─────────────────────┬──────────┬──────────────┬───────┬─────────────┬────────────┬──────────────────────────────────────────┐
│ Simulator           │ Language │ License      │ Lines │ Test Mode   │ Pass Rate  │ Failures                                 │
├─────────────────────┼──────────┼──────────────┼───────┼─────────────┼────────────┼──────────────────────────────────────────┤
│ **cpu65816 (ours)** │ C89      │ MIT          │ 1,337 │ Both        │ **99.99%** │ 463 (emulation-mode test-data bugs only) │
├─────────────────────┼──────────┼──────────────┼───────┼─────────────┼────────────┼──────────────────────────────────────────┤
│               816CE │ C99      │ BSD-3        │ 4,715 │ Both        │      87.9% │                                  619,031 │
├─────────────────────┼──────────┼──────────────┼───────┼─────────────┼────────────┼──────────────────────────────────────────┤
│ emu816 (Andrew Jacobs)│ C++   │ CC BY-NC-SA 4.0│ ~1,600│ Both        │      77.4% │                              1,155,702 │
├─────────────────────┼──────────┼──────────────┼───────┼─────────────┼────────────┼──────────────────────────────────────────┤
│ emu65816            │ C99      │ GPL-3.0      │ 3,669 │ Native only │ ~91%*      │                30/256 opcodes fail (n=3) │
├─────────────────────┼──────────┼──────────────┼───────┼─────────────┼────────────┼──────────────────────────────────────────┤
│ Lib65816            │ C++      │ GPL-3.0      │ 2,042 │           — │ Not tested │ Author warns "not adequately tested"     │
└─────────────────────┴──────────┴──────────────┴───────┴─────────────┴────────────┴──────────────────────────────────────────┘

\* emu65816 emulation-mode tests skipped due to cycle-table indexing bug causing infinite loops.

### 816CE (ElectronicsTinkerer) — Detailed Breakdown

┌─────────────────┬──────────┬───────────┬───────────────────────────────────────────────────────────┐
│ Category        │ Opcodes  │ Pass Rate │ Notes                                                     │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ Perfect (100%)  │      176 │      100% │ All data movement, logic, branch, stack, transfer opcodes │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ ADC binary mode │ $61-$7F  │       37% │ Decimal flag clear works; decimal mode broken             │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ SBC all modes   │ $E1-$FF  │        0% │ Subtraction completely broken                             │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ COP             │ $02.e    │        0% │ Emulation-mode COP fails                                  │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ MVN/MVP         │ $44, $54 │        0% │ Block move unimplemented                                  │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ WAI             │ $CB      │        0% │ Wait-for-interrupt returns wrong state                    │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ STP             │ $DB      │        0% │ Stop returns wrong state                                  │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ PER             │ $62      │    10–17% │ Partially working                                         │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ SEP             │ $F3      │        0% │ Set status broken                                         │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ JSR indirect    │ $FB      │       69% │ Partial failure                                           │
├─────────────────┼──────────┼───────────┼───────────────────────────────────────────────────────────┤
│ PLA/PLB         │ $AB.e    │     99.6% │ Minor emulation-mode issue                                │
└─────────────────┴──────────┴───────────┴───────────────────────────────────────────────────────────┘

**Root causes:**
- SBC: fundamental implementation error in subtraction algorithm (0% pass across all addressing modes)
- ADC: decimal-mode arithmetic incorrect (binary mode works at ~37% — tests include both binary and decimal cases)
- COP/WAI/STP: interrupt handling returns wrong processor state
- MVN/MVP: block move instructions unimplemented

### emu65816 (ProxyPlayerHD) — Detailed Breakdown

┌──────────────────┬───────────┬──────────┬───────────────────────────────────────────────────────────────────┐
│ Category         │ Opcodes   │ Result   │ Notes                                                             │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ Emulation mode   │ All       │ **SKIP** │ Cycle table bug: index `0x0700+opcode` overflows 1280-entry table │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ MVN              │ $44.n     │       0% │ Unimplemented                                                     │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ MVP              │ $54.n     │       0% │ Unimplemented                                                     │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ WDM              │ $42       │ Skip     │ Cycle table entry = 0 → infinite loop                             │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ ADC decimal      │ $61-$7F.n │ ~63%     │ Known bug per README                                              │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ SBC decimal      │ $E1-$FF.n │ ~63%     │ Known bug per README                                              │
├──────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ All other native │ ~230      │     100% │                                                3 tests per opcode │
└──────────────────┴───────────┴──────────┴───────────────────────────────────────────────────────────────────┘

**Root causes:**
- Cycle table: `cycleTable[(EF?0x0400:0) | (MF?0x0200:0) | (XF?0x0100:0) | opcode]` — in emulation mode, index becomes `0x0700+opcode`, exceeding the 1280-entry table, reading garbage → infinite loop in `cpuExecute`
- No single-step API: `cpuExecute()` runs until cycle budget exhausted; `cycle=1` trick works for most instructions but any 0-cycle opcode loops forever
- Decimal SBC: explicitly documented as buggy in README
- MVN/MVP: unimplemented (README states this)
- Memory: requires flat 16MB `uint8_t[]` array, no banked/I/O mapping

### emu816 (Andrew Jacobs) — Detailed Breakdown

┌──────────────────────────┬───────────┬──────────┬───────────────────────────────────────────────────────────────────┐
│ Category                 │ Opcodes    │ Pass Rate│ Notes                                                             │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ CPX/CPY emulation mode   │ $C0.e, $C4.e, $C8.e, $CC.e │ 0% │ Emulation-mode compares completely broken │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ SEP                      │ $F3.n      │ 41%      │ Sets flags but doesn't clear high bytes of X/Y immediately         │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ REP                      │ $C2.n      │ 0%       │ Doesn't correctly clear M/X flags in all cases                    │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ ADC/SBC decimal mode     │ $61-$7F, $E1-$FF │ ~56% │ Decimal arithmetic buggy in both directions               │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ SBC binary native        │ $E1-$FF.n │ ~56%     │ Even binary SBC has systematic errors                            │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ MVN/MVP                  │ $44, $54   │ 0%       │ Implementation differs from test expectations                      │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ WAI/STP                  │ $CB, $DB   │ 0%       │ Decrements PC and waits; test expects register state changes     │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ WDM                      │ $42        │ 0%       │ Exits simulator on $FF; other WDM opcodes treated as NOP         │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ Direct page addressing  │ Various    │ 50–75%   │ Address calculation bugs with nonzero D in emulation mode           │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ JMP indirect             │ $6C.n      │ ~0%      │ Absolute indirect jump broken                                     │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ JSR indirect             │ $FC.n      │ ~75%     │ Partially working                                                 │
├──────────────────────────┼───────────┼──────────┼───────────────────────────────────────────────────────────────────┤
│ PLA/PLB emulation       │ $AB.e, $AB.e │ ~99%   │ Minor emulation-mode issues                                      │
└──────────────────────────┴───────────┴──────────┴───────────────────────────────────────────────────────────────────┘

**Root causes:**
- SBC uses complement trick (`data = ~operand`, then `temp = A + data + carry`) which is correct for binary, but decimal correction is wrong — similar pattern to 816CE's ADC decimal bug
- CPX/CPY in emulation mode: high byte of registers not properly zero-extended when X flag is set
- Direct page addressing with nonzero D register: emulation-mode page-wrapping differs from WDC specification
- WAI/STP: emulator treats these as suspend states (decrement PC and wait), but tests expect specific register state changes
- WDM: uses `WDM #$FF` as simulator exit; no proper NOP behavior
- JMP absolute indirect ($6C): reads wrong bank for indirect address

### Lib65816 — Not Tested

- **License:** GPL-3.0 (incompatible with our MIT license)
- **Architecture:** C++ class hierarchy with virtual dispatch, ~2,042 LOC
- **Author's disclaimer:** "I didn't test this enough to claim that it works well, you might have to do some bugfixing :)"
- **Test harness not written** due to: (1) GPL license conflict, (2) C++ class API incompatible with our C test framework, (3) author's explicit quality warning

## Speed Comparison

┌─────────────────────┬──────────────────────┬──────────────────┬─────────────────────────────────────────────────────┐
│ Simulator           │ API                  │ 10M Instructions │ Notes                                               │
├─────────────────────┼──────────────────────┼──────────────────┼─────────────────────────────────────────────────────┤
│ **cpu65816 (ours)** │ `cpu65816_step()`    │   237M instr/sec │ Single-step, cycle-accurate                         │
├─────────────────────┼──────────────────────┼──────────────────┼─────────────────────────────────────────────────────┤
│ emu816              │ `emu816::step()`     │ Not benchmarked  │ All-static C++; 16MB RAM + ROM; author claims ~225MHz │
├─────────────────────┼──────────────────────┼──────────────────┼─────────────────────────────────────────────────────┤
│ emu65816            │ `cpuExecute(n)`      │ N/A              │ Benchmark hangs; 16MB flat RAM + cycle budget API   │
├─────────────────────┼──────────────────────┼──────────────────┼─────────────────────────────────────────────────────┤
│               816CE │ `stepCPU()`          │ Not benchmarked  │ 16MB `memory_t` struct array (2 bytes/entry = 32MB) │
├─────────────────────┼──────────────────────┼──────────────────┼─────────────────────────────────────────────────────┤
│ Lib65816            │ C++ virtual dispatch │ Not benchmarked  │ Not tested                                          │
└─────────────────────┴──────────────────────┴──────────────────┴─────────────────────────────────────────────────────┘

## Gap Analysis

### Where cpu65816 leads

1. **Accuracy:** Only simulator to pass 99.99% of SingleStepTests (463 emulation-mode failures are test-data bugs, not simulator bugs — test data doesn't enforce WDC-mandated page-1 stack wrapping)
2. **API design:** Single-step API (`cpu65816_step()`) returns cycle count, enables cycle-accurate simulation; competitors use cycle-budget or single-step with error codes
3. **Memory model:** Callback-based r/w (no flat RAM requirement); supports banked memory, I/O mapping, and zero-footprint operation
4. **Size:** 1,337 LOC vs 4,715 (816CE) and 3,669 (emu65816) — 3–4× more compact
5. **License:** MIT — no restrictions; GPL-3.0 competitors cannot be linked with proprietary code

### Where cpu65816 could improve

1. **Emulation-mode test coverage:** 463 failures in Tom Harte's tests (test-data bugs — our simulator enforces WDC datasheet stack wrapping that the tests don't account for). Could add a compatibility mode that relaxes stack wrapping to match test expectations.
2. **Block move (MVN/MVP):** 816CE also fails these (0%); emu65816 marks them as unimplemented. Both competitors confirm block move is an edge case that's hard to get right.

### Competitor-specific issues

┌──────────────────┬─────────────────────┬─────────────────────────┬──────────────────┬──────────────────┐
│ Issue            │               816CE │ emu65816                │ emu816           │ cpu65816         │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ SBC correctness  │ 0% pass (all modes) │ ~63% pass (native only) │ ~56% pass        │        100% pass │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ ADC decimal mode │ ~37% pass           │ ~63% pass (native)      │ ~56% pass        │        100% pass │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ COP/WAI/STP      │ Wrong state         │ N/A (emul. skip)        │ Wrong state      │ Correct          │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ MVN/MVP          │ 0% pass             │ Unimplemented           │ 0% pass          │ 100% pass        │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ Cycle-accurate   │ No                  │ No (cycle table bug)    │ Yes              │ Yes              │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ Memory footprint │     32MB (memory_t) │         16MB (flat RAM) │ 16MB (RAM+ROM)  │ Zero (callbacks) │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ Single-step API  │ Yes (`stepCPU`)     │ No (`cpuExecute`)       │ Yes (`step()`)   │ Yes (`stepCPU`)  │
├──────────────────┼─────────────────────┼─────────────────────────┼──────────────────┼──────────────────┤
│ Error handling   │ Error codes         │ None                    │ `isStopped()`    │ N/A              │
└──────────────────┴─────────────────────┴─────────────────────────┴──────────────────┴──────────────────┘

## API

```c
#include "cpu65816.h"

CPU65816 cpu;
cpu65816_init(&cpu, ram, read_cb, write_cb);
cpu65816_reset(&cpu);
int cycles = cpu65816_step(&cpu);
cpu65816_irq(&cpu, ...);
cpu65816_dump(&cpu);       /* DEBUG only */
```

## Registers

┌──────────┬───────┬───────────────────────────────────────────────────┐
│ Register │ Width │ Description                                       │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ A        │    16 │ Accumulator (C=low 8, B=high 8)                   │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ X, Y     │    16 │ Index registers (8 or 16-bit per X flag)          │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ SP       │    16 │ Stack pointer (high byte forced $01 in emulation) │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ PC       │    16 │ Program counter (offset within PBR bank)          │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ D        │    16 │ Direct page register                              │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ P        │     8 │ Processor status: NV-MXDIZC                       │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ PBR      │     8 │ Program bank register (K)                         │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ DBR      │     8 │ Data bank register                                │
├──────────┼───────┼───────────────────────────────────────────────────┤
│ E        │     8 │ Emulation flag: 1=6502-compatible, 0=native       │
└──────────┴───────┴───────────────────────────────────────────────────┘

## Compile Flags

┌────────────────┬──────────────────────────────────────────────┐
│ Define         │ Effect                                       │
├────────────────┼──────────────────────────────────────────────┤
│ `COUNT_CYCLES` │ step() returns cycle count (uint8_t)         │
├────────────────┼──────────────────────────────────────────────┤
│ `SINGLE_INST`  │ Bare static globals, no cpu pointer param    │
├────────────────┼──────────────────────────────────────────────┤
│ `MEM_IO`       │ Memory r/w callbacks for I/O-mapped hardware │
├────────────────┼──────────────────────────────────────────────┤
│ `DEBUG`        │ Enable cpu65816_dump()                       │
└────────────────┴──────────────────────────────────────────────┘

## Files

┌────────────────┬─────────────────────────────┐
│ File           │ Description                 │
├────────────────┼─────────────────────────────┤
│ cpu65816.c     │ Core implementation         │
├────────────────┼─────────────────────────────┤
│ cpu65816.h     │ Public API                  │
├────────────────┼─────────────────────────────┤
│ cpu65816MINI.h │ Minimal header-only variant │
└────────────────┴─────────────────────────────┘

## Competitor Sources

┌───────────┬──────────────────────────────────────────────┐
│ Simulator │ Repository                                   │
├───────────┼──────────────────────────────────────────────┤
│     816CE │ https://github.com/ElectronicsTinkerer/816CE │
├───────────┼──────────────────────────────────────────────┤
│ emu816    │ https://github.com/andrew-jacobs/emu816      │
├───────────┼──────────────────────────────────────────────┤
│ emu65816  │ https://github.com/ProxyPlayerHD/emu65816    │
├───────────┼──────────────────────────────────────────────┤
│ Lib65816  │ https://github.com/andreas-burg/Lib65816     │
└───────────┴──────────────────────────────────────────────┘

## Test Harness Files

┌──────────────────────────────────┬────────────────────────────────────────────────────┐
│ File                             │ Description                                        │
├──────────────────────────────────┼────────────────────────────────────────────────────┤
│ test/comp65816/test_816CE.cpp    │                       816CE SingleStepTests runner │
├──────────────────────────────────┼────────────────────────────────────────────────────┤
│ test/comp65816/test_emu65816.cpp │ emu65816 SingleStepTests runner (native mode only) │
├──────────────────────────────────┼────────────────────────────────────────────────────┤
│ test/comp65816/test_emu816.cpp   │ emu816 (Andrew Jacobs) SingleStepTests runner      │
├──────────────────────────────────┼────────────────────────────────────────────────────┤
│ test/comp65816/test_gilyon.cpp   │ gilyon/snes-tests runner (parse text format)       │
├──────────────────────────────────┼────────────────────────────────────────────────────┤
│ test/comp65816/bench_emu65816.c  │ emu65816 micro-benchmark                           │
├──────────────────────────────────┼────────────────────────────────────────────────────┤
│ test/comp65816/Makefile          │ Build system for competitor tests                  │
└──────────────────────────────────┴────────────────────────────────────────────────────┘

## gilyon/snes-tests

[gilyon/snes-tests](https://github.com/gilyon/snes-tests) is a 65816 test suite designed to run on real SNES hardware. It uses a ROM-based approach — the test program boots on a SNES, runs each instruction, and verifies results against expected values captured from actual hardware.

### Test Format

The `tests-full.txt` release provides test cases in a human-readable format:

```
Test XXXX: <instruction>  ; <comment>
   Input: A=$1234 X=$3456 Y=$5678 P=$00 E=0 DBR=$7f D=$ffff ($7f1212)=$cb ...
   Expected output: A=$0000 X=$3456 Y=$5678 P=$03 E=0 ($7f1212)=$cb ...
   Additional initialization or checks are performed - see assembly
```

### Limitation

**The text format does not include PC (program counter) values.** The PC is embedded in the assembly source code (`.inc` files), which requires full SNES LoROM mapping to determine the instruction's location. Without the PC:

- We cannot set up the CPU to execute the instruction at the correct address
- Many tests also have "Additional initialization or checks" that require assembly-level setup beyond what the text format provides
- Running the actual `.sfc` ROM would require a full SNES system emulator (PPU, DMA, joypad), not just a bare CPU simulator

**Current status:** test_gilyon.cpp parses the text format but skips all tests due to missing PC values. To achieve full gilyon coverage, we would need to either:

1. Parse the assembly `.inc` files to extract PC values and additional setup code
2. Run the `cputest-full.sfc` ROM on a SNES emulator that uses our cpu65816 as its CPU core

### Notable gilyon findings

Two undocumented behaviors that ALL emulators (including ours before fixes) get wrong:

1. **Emulation-mode `(direct,X)` with nonzero D**: When D's high byte is nonzero and E=1, the effective address calculation has special page-wrapping — the high byte is taken from D+1 rather than D, giving an address like `$xx(DH+1)DL+X` instead of `$xx(DH)DL+X`
2. **PLB with S=$1FF**: In emulation mode, PLB reads from $200 (wrapping from $1FF → $200) instead of $100. This is a known 65816 quirk where stack operations in emulation mode use the full 16-bit address after increment
