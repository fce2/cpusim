## (yet another) 6502 simulator (V3)

"T" for Transistor.

"Engine T" — transistor-level simulation.
Simulates the real 6502 "silicon" at the transistor/netlist level using the visual6502 netlist data (many thanks to James / Silverman / Silverman).
(Kind of) drop-in replacement for cpu6502.h.
Same struct, same function signatures (but more).
Not optimized for speed, only for accuracy.
6502M is the smallest simulator, 6502T is the slowest (well, cuda is !).

- Only NMOS 6502
- Illegal opcodes handled naturally by the netlist

Since this is 100% silicon compatible, with all bugs and quirks, it's a perfect test generator !
See main004.cpp to generate Tom Harte compatible "perfect zips" (named after perfect6502). As much as you need !
See main005.cpp for testing Tom Harte zips or the perfect zips.
Sorry for the mainT012345abctxyz scheme, thats obviously how my brains netlist works.
One "bug" i found in my cpu6502.c/h this way: I flag is set after reset on real cpus.

### source files

#### the simulator itself

-DENGINE_T
to replace cpu with T-model

- `6502T/cpu6502T.h`
- `6502T/cpu6502T.c`
- `6502T/netlist_6502.h` - the "real" 3510 transistors
- `sim.h`

-DENGINE_CUDA
to replace cpu with gpu

- `6502T/cpu6502CUDA.h`
- `6502T/cpu6502CUDA.cu`
- `6502T/netlist_6502.h`

### testing

---

- `main001.cpp`
	just some cpu steps, but in 3 variants
		main001.exe uses cpu6502.c/h
		main001t.exe uses cpu6502T.c/h
		main001c.exe uses the cuda variant (nvidia only)

---

- `main002.cpp`

|     Engine     |     MIPS     |      Speed vs C      |
|----------------|--------------|----------------------|
| cpu6502.c      | ~450 MIPS    | baseline             |
| cpu6502T.c     | ~0.006 MIPS  | ~75,000× slower      |
| cpu6502CUDA.cu | ~0.0003 MIPS | ~1.3 million× slower |

- `main002.exe`
	using `6502/cpu6502.c/h`
	"boot" the C64 to READY.
	a little benchmark ...
	the good.
```
took 1.797ms
  422.54646633 MIPS
  1506.37451308 MCPS
2706955 cycles, 159 IRQs
PC=E5CD A=00 X=00 Y=0A P=22:nvUbdiZc SP=F3
Jiffy: $04 $00 $00

    **** COMMODORE 64 BASIC V2 ****

 64K RAM SYSTEM  51216 BASIC BYTES FREE

READY.
```

51216 bytes free becuse no no proper ram management.

- `main002t.exe`
	using `6502T/cpu6502T.c/h`
	... vs cpu6502T.c/h 
	the bad.
	the transistor sim in C is slow because it has to be - it's evaluating 3,510 transistors per cycle to faithfully model the silicon.
```
[6502T] Running 1000000 cycles...
  [##############################] 100%  1000005/1000000    0.02 MIPS  ETA   0s
took 50973.063ms
  0.00549035 MIPS
  0.01961830 MCPS
1000005 cycles, 58 IRQs
PC=FD75 A=55 X=00 Y=AF P=25:nvUbdIzC SP=FD
Jiffy: $00 $00 $00
```
not enough cycles given to show "boot screen".
would take some minutes at 0.005 MIPS !

- `main002c.exe`
	using `6502T/cpu6502CUDA.cu/h`
	... vs cuda
	surprise ! 0.0003 MIPS on a RTX4090 !
	the ugly.
	the netlist is a deeply sequential state machine. there's no good exploitable parallelism.
	and 3510 transistors is "nothing" for a gpu.
```
[6502C] Running 100000 cycles...
  [##############################] 100%   100000/100000    0.00 MIPS  ETA   0s
took 90166.900ms
  0.00030889 MIPS
  0.00110905 MCPS
100000 cycles, 5 IRQs
PC=FD84 A=00 X=00 Y=64 P=27 SP=FD
Jiffy: $00 $00 $00
```
not enough cycles given to show "boot screen".
would take some more minutes at 0.0003 MIPS !

---

- `main003`
some simple benchmarks

**6502**: cpu6502.c/h
```
Workload                    Insns            MIPS   Time(ms)
Counter DEX+BNE          10000000     377.5251715    26.4883
Fibonacci(20)            10000000     375.7096215    26.6163
Idle JMP-self            10000000     374.6468953    26.6918
MemFill 256B             10000000     376.3884027    26.5683
Copy LDAiy+STAiy         10000000     378.5082988    26.4195
Multiply 8bit            10000000     377.0696410    26.5203
```

**6502T**: cpu6502T.c/h
```
Workload                    Insns            MIPS   Time(ms)
Counter DEX+BNE             10000       0.0032171  3108.4122
Fibonacci(20)               10000       0.0032770  3051.5462
Idle JMP-self               10000       0.0032538  3073.2985
MemFill 256B                10000       0.0032819  3047.0408
Copy LDAiy+STAiy            10000       0.0032634  3064.2685
Multiply 8bit               10000       0.0032846  3044.5005
```

**6502C**: cpu6502CUDA.cu/h
```
Workload                    Insns            MIPS   Time(ms)
Counter DEX+BNE              1000       0.0001883  5312.0360
Fibonacci(20)                1000       0.0001951  5126.3454
Idle JMP-self                1000       0.0001957  5109.4650
MemFill 256B                 1000       0.0001953  5119.6000
Copy LDAiy+STAiy             1000       0.0001957  5108.5652
Multiply 8bit                1000       0.0001935  5169.1468
```

---

- `main004`
generate Tom Harte compatible "perfect zips" (named after perfect6502).
maybe the only useful thing in this directory...
since i only have one transistor list, only this cpu type is generated.
toms zip supports 5 variants, so its much bigger.
it takes a while to create a 10k zip, so i prepared it: `6502T/65x02-perfect.zip`.
if you need reproducable zips, use "--seed <u64>".

---

- `main005`
testing "cpu6502.c/h" on Tom Harte zips or the perfect zips.

---
