## (yet another) 6502 simulator — eXtreme edition (V4)

"X" for eXtreme (or eXperiment).

WIP and prototype area here !
But better releasing already workable stuff than waiting for 99% beautyful source code (which will never come).
One of the first comments was "warum bloß !" ('but why ??').
Well, i'm curious, its getting more and more theroetical now.
Getting a bit absurd here, stop reading if it hurts !
The maybe only useful thing in this repo is `6502T/main004.cpp`.
This was (and is) an interesting journey so far.

**Some records:**
- The smallest 6502 sim source code is here: `6502M/cpu6502M.h` (**4916 bytes**)
- The smallest 6502 sim binary is here: `6502/main000.prg` (**5591 bytes**)
- The slowest 6502 sim is here: `6502T/main003c.exe` (**0.00018 MIPS**, thats 180 IPS, no M, no K, raw Instructions Per Second)
- The fastest 6502 sim is here: `6502X/main001ultra.exe` (**1.4 TIPS**, the one after GIPS which is the one after MIPS, thats 1400000000000 IPS)

### '100K' engine

100,000 independent 6502 instances on GPU, each with its own 64KB RAM.
100K was chosen because: <8GB VRAM (runs "everywhere") with full RAM per core minus safety margin.
~90% of VRAM usable for instances, rest reserved for CUDA context, display, kernels.

| Max instances | RAM usage |
|---------------|-----------|
| 58,974 | 3.6 GB |
| 117,949 | 7.2 GB |
| 176,924 | 10.8 GB |
| 235,899 | 14.4 GB |
| 353,850 | 21.6 GB |
| 471,800 | 28.8 GB |

#### V'max': ~830 GIPS

**Can we get more GIPS ?**
Reduce 64k RAM to 2k only, but more cpus.
Enough for a (synthetic) speed-test at least.

**Result:**
The 2k RAM now fits the 4090's L2 cache, so the per-step global-VRAM traffic drops and we escape the ~500 GIPS VRAM-bandwidth rate — ~830 GIPS.
That ~500 GIPS is still the RTX 4090's VRAM bandwidth: ~1008 GB/s VRAM, each 6502 step touches ~2 global bytes on average (opcode + operand), 1008/2 ≈ 504.

#### V'ultra': 1.39040 TIPS / 19.75 MIPS

**I want to have more GIPS !**
Shrink RAM to 4b ('bytes', yes, just enough for "JMP $0000" 4C 00 00).
And yes, we could go to 3... Or just 1 with NOP or BRK. Maybe next time.
But use 2.083B 6502-cores (**2 billion** ;-).

**Result:**
Another debunked optimization hypothesis.
~40 TIPS raw int32 4090 speed. -> GPU roofline (128 cores/SM × 128 SM × 2.52 GHz), reachable only by 1-instr counting work, not by faithful simulation.
~5-20 TIPS in "guessed" 6502 sim worload. -> 
1.4 TIPS is ceiling here for now.

I'd love to see results for a B200 !
Or test one myself, so please send me your spare cards (anything >=H100).

> what could we reach on a B200 cluster ?
< ~1,000 B200s (multi-rack): **~1.5 PIPS**

If you want to know: IPS, KIPS, MIPS, GIPS, TIPS, PIPS, EIPS, ZIPS, YIPS, RIPS, QIPS, ...
And the other direction: IPS, mIPS, µIPS, nIPS, pIPS, fIPS, aIPS, zIPS, yIPS, rIPS, qIPS, ...

#### whats after ultra ?

Yes, V5 exists also already now, but too much WIP...

For 6502: back from ultra-broad to brutal single code speed.
I'm working on a "block-jit" simulator, drop-in replacement for `6502/cpu6502.c/h`.
Already 3-4x speed of my `cpu6502.c/h`, ~1.5-2 GIPS on average load instead of 500 MIPS.
Up to 50 TIPS on some "well treated" sw-parts already, mainly detected and unrolled loops and things like that, NOP is of course a nop ;-)
cpu6502X.h, sim6581X.h, sim6567X.h, sim6526X.h are on my testbench now (if i find more time and motivation).
(Ja, noch so eine "warum bloß !" Aktion...)

And I've some more CPUs...

### source files

#### CUDA 100K mass-parallel

- `main.h` — little helpers

- `6502X/cpu6502CUDA100k.h` / `cpu6502CUDA100k.cu` — 'many' parallel 6502s on GPU (instruction-level, not transistor)

### build defines

| Define | Effect |
|--------|--------|
| `ENGINE_T` | use transistor-level engine (cpu6502T, in 6502T/) |
| `ENGINE_CUDA` | use CUDA netlist engine (cpu6502CUDA, in 6502T/) |
| `NUMCPUS` | number of parallel 6502 instances (default 100000) |

### testing

- `main001.cpp` — multi-engine benchmark: 6 micro-benchmarks across all engines
	- `main001.exe` — instruction-level (cpu6502.c)
```
=== 6502 ===
Workload                    Insns             MIPS   Time(ms) Result
--------------------------------------------------------------------
Counter DEX+BNE          10000000       375.303527    26.6451 PASS PC=$0305
Fibonacci(20)            10000000       380.485652    26.2822 PASS PC=$021B
Idle JMP-self            10000000       389.532483    25.6718 PASS PC=$0400
MemFill 256B             10000000       391.417008    25.5482 PASS PC=$050A
Copy LDAiy+STAiy         10000000       381.908243    26.1843 PASS PC=$0609
Multiply 8bit            10000000       387.832906    25.7843 PASS PC=$071E
```
	- `main001t.exe` — transistor-level (cpu6502T.c)
```
=== 6502T ===
Workload                    Insns             MIPS   Time(ms) Result
--------------------------------------------------------------------
Counter DEX+BNE             10000         0.009020  1108.6877 PASS PC=$0305
Fibonacci(20)               10000         0.003357  2978.6864 PASS PC=$0000
Idle JMP-self               10000         0.009210  1085.7823 PASS PC=$0400
MemFill 256B                10000         0.008299  1205.0302 PASS PC=$050A
Copy LDAiy+STAiy            10000         0.008128  1230.3077 PASS PC=$0609
Multiply 8bit               10000         0.008632  1158.5212 PASS PC=$071E
```
	- `main001c.exe` — transistor-level CUDA (cpu6502CUDA.cu)
```
=== 6502C ===
Workload                    Insns             MIPS   Time(ms) Result
--------------------------------------------------------------------
Counter DEX+BNE              1000         0.000190  5256.0680 PASS PC=$0000
Fibonacci(20)                1000         0.000195  5131.0901 PASS PC=$0000
Idle JMP-self                1000         0.000194  5158.2749 PASS PC=$0000
MemFill 256B                 1000         0.000195  5118.1409 PASS PC=$0000
Copy LDAiy+STAiy             1000         0.000195  5115.6811 PASS PC=$0000
Multiply 8bit                1000         0.000195  5137.0693 PASS PC=$0000
```
	- `main001max.exe` — 11M normal cores
```
=== CUDA 100K 6502 Benchmark ===
Instances: 11000000  RAM: 2KB  Warmup: 100  Measure: 10000

CPU100k V2: 11000000 instances ├ù 2KB, 83.9 MB regs + 21484.4 MB RAM = 21568.3 MB total (interleaved+ZPcache)
Workload              Insns/GPU       GIPS  MIPS/inst   Time(ms)
--------------------------------------------------------------------------
Counter DEX+BNE      110000000000   878.3459     0.0798   125.2354
Fibonacci(20)        110000000000   888.7402     0.0808   123.7707
Idle JMP-self        110000000000   897.9724     0.0816   122.4982
MemFill 256B         110000000000   832.8740     0.0757   132.0728
Copy LDAiy+STAiy     110000000000   760.7124     0.0692   144.6013
Multiply 8bit        110000000000   881.9042     0.0802   124.7301
```
	- `main001ultra.exe` — 2B micro cores
```
=== CUDA ULTRA 6502 Benchmark (full RAM on-chip shared) ===
Instances: 2083000000  RAM: 4B  Warmup: 100  Measure: 10000

CPU100k V2: 2083000000 instances ├ù 0KB, 15892.0 MB regs + 7946.0 MB RAM = 23838.0 MB total (interleaved+ZPcache)
Workload              Insns/GPU       GIPS  MIPS/inst   Time(ms)
--------------------------------------------------------------------------
Idle JMP-self        20830000000000  1405.6369     0.0007 14818.9056
```

---
