ROOT = ..
LIBS = C:/Users/g/claude/libs
CXX  = g++
# NOTE: Makefile_X is the SOURCE Makefile (in 6502/). It gets deployed to 6502X/Makefile by syncweb.bat.
# In the source dir (6502/), all .c/.h files are local (no ../6502/ prefix needed).
# In the deployed dir (6502X/), files come from ../6502/ and ../6502T/.
# This Makefile uses deployed paths (../6502/, ../6502T/) which work from both locations
# because 6502/ has the same files locally and 6502X/ references them via ../.
CFLAGS = -O2 -I. -I$(ROOT) -I$(ROOT)/libs -I$(ROOT)/retrolib -I$(ROOT)/6502 -I$(ROOT)/6502T -I$(LIBS)/retrolib -DDEBUG -DCOUNT_CYCLES -DILLEGAL
NVCC = nvcc
NVCCFLAGS = -std=c++17 -O2 -I. -I$(ROOT) -I$(ROOT)/libs -I$(ROOT)/retrolib -I$(ROOT)/6502 -I$(ROOT)/6502T -I$(LIBS)/retrolib -arch=sm_89
PETSCII = $(ROOT)/retrolib/cbm_petscii.c

.PHONY: all cuda clean test

all: main001.exe main001t.exe main001c.exe main001max.exe main001ultra.exe

cuda: main001c.exe bench10k.exe bench50k.exe bench100k.exe bench200k.exe bench300k.exe bench400k.exe bench500k.exe benchmax.exe main001max.exe main001ultra.exe

# === Standard cpu6502.c (instruction-level) ===
main001.exe: main001.cpp main.h $(ROOT)/6502/cpu6502.h $(ROOT)/6502/cpu6502.c
	$(CXX) $(CFLAGS) -o $@ main001.cpp $(ROOT)/6502/cpu6502.c

# === Transistor-level cpu6502T ===
main001t.exe: main001.cpp main.h $(ROOT)/6502T/cpu6502T.h $(ROOT)/6502T/cpu6502T.c $(ROOT)/6502T/netlist_6502.h $(ROOT)/libs/compress.c
	$(CXX) $(CFLAGS) -DENGINE_T -DTHREADSAFE -o $@ main001.cpp $(ROOT)/6502T/cpu6502T.c $(ROOT)/libs/compress.c

# === CUDA transistor cpu6502CUDA (requires CUDA + vcvars64) ===
main001c.exe: main001.cpp main.h $(ROOT)/6502T/cpu6502CUDA.h $(ROOT)/6502T/cpu6502CUDA.cu
	-$(NVCC) $(NVCCFLAGS) -DENGINE_CUDA -o $@ main001.cpp $(ROOT)/6502T/cpu6502CUDA.cu

# === CUDA mass-parallel (requires CUDA + vcvars64) ===
NVCC100K = -std=c++17 -O2 -DBENCH_CUDA100K -DILLEGAL -I. -I$(ROOT) -I$(ROOT)/libs -I$(ROOT)/retrolib -I$(ROOT)/6502 -I$(ROOT)/6502T -I$(LIBS)/retrolib -arch=sm_89

bench10k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=10000 -o $@ main001.cpp cpu6502CUDA100k.cu

bench50k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=50000 -o $@ main001.cpp cpu6502CUDA100k.cu

bench100k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=100000 -o $@ main001.cpp cpu6502CUDA100k.cu

bench200k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=200000 -o $@ main001.cpp cpu6502CUDA100k.cu

bench300k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=300000 -o $@ main001.cpp cpu6502CUDA100k.cu

bench400k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=400000 -o $@ main001.cpp cpu6502CUDA100k.cu

bench500k.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DNUMCPUS=500000 -o $@ main001.cpp cpu6502CUDA100k.cu

# === CUDA MAX: 11M instances x 2KB (RTX 4090 90% VRAM) ===
benchmax.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DMAX -o $@ main001.cpp cpu6502CUDA100k.cu

main001max.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DMAX -o $@ main001.cpp cpu6502CUDA100k.cu

# === CUDA ULTRA: minimal RAM (4B), max instances (~1.8B, ~90% VRAM), full RAM on-chip shared ===
main001ultra.exe: main001.cpp cpu6502CUDA100k.h cpu6502CUDA100k.cu
	-$(NVCC) $(NVCC100K) -DULTRA -o $@ main001.cpp cpu6502CUDA100k.cu

test: all
	./main001.exe
	./main001t.exe
	-./main001c.exe || true

clean:
	rm -f main001.exe main001t.exe main001c.exe bench10k.exe bench50k.exe bench100k.exe bench200k.exe bench300k.exe bench400k.exe bench500k.exe benchmax.exe main001max.exe main001ultra.exe