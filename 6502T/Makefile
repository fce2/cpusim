ROOT = ..
LIBS = $(ROOT)/libs
PETSCII = $(ROOT)/retrolib/cbm_petscii.c
CXX  = g++
CFLAGS = -O2 -I$(ROOT) -I$(LIBS) -I$(ROOT)/retrolib -DDEBUG -DCOUNT_CYCLES -DMEM_IO
NVCC = nvcc
NVCCFLAGS = -std=c++17 -O2 -I$(ROOT) -I$(LIBS) -I$(ROOT)/retrolib -arch=sm_89
#call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

.PHONY: all clean cuda test

all: main001.exe main002.exe main003.exe main004.exe main005.exe t

main001.exe: main001.cpp main.h $(ROOT)/6502/cpu6502.h $(ROOT)/6502/cpu6502.c
	$(CXX) $(CFLAGS) -I$(ROOT)/6502 -o $@ main001.cpp $(ROOT)/6502/cpu6502.c

main002.exe: main002.cpp main.h $(ROOT)/6502/cpu6502.h $(ROOT)/6502/cpu6502.c $(PETSCII)
	$(CXX) $(CFLAGS) -I$(ROOT)/6502 -o $@ main002.cpp $(ROOT)/6502/cpu6502.c $(PETSCII)

main003.exe: main003.cpp main.h $(ROOT)/6502/cpu6502.h $(ROOT)/6502/cpu6502.c
	$(CXX) $(CFLAGS) -I$(ROOT)/6502 -o $@ main003.cpp $(ROOT)/6502/cpu6502.c

main004.exe: main004.cpp main.h cpu6502T.h cpu6502T.c $(LIBS)/compress.h $(LIBS)/compress.c
	$(CXX) $(CFLAGS) -DTHREADSAFE -o $@ main004.cpp cpu6502T.c $(LIBS)/compress.c

main005.exe: main005.cpp $(ROOT)/6502/cpu6502.h $(ROOT)/6502/cpu6502.c $(LIBS)/compress.c $(LIBS)/json.c
	$(CXX) $(CFLAGS) -DILLEGAL -I$(ROOT)/6502 -o $@ main005.cpp $(ROOT)/6502/cpu6502.c $(LIBS)/compress.c $(LIBS)/json.c

main001t.exe: main001.cpp main.h cpu6502T.h cpu6502T.c
	$(CXX) $(CFLAGS) -DENGINE_T -o $@ main001.cpp cpu6502T.c

main002t.exe: main002.cpp main.h cpu6502T.h cpu6502T.c $(PETSCII)
	$(CXX) $(CFLAGS) -DENGINE_T -o $@ main002.cpp cpu6502T.c $(PETSCII)

main003t.exe: main003.cpp main.h cpu6502T.h cpu6502T.c
	$(CXX) $(CFLAGS) -DENGINE_T -o $@ main003.cpp cpu6502T.c

main001c.exe: main001.cpp main.h cpu6502CUDA.h cpu6502CUDA.cu
	-$(NVCC) $(NVCCFLAGS) -DENGINE_CUDA -o $@ main001.cpp cpu6502CUDA.cu

main002c.exe: main002.cpp main.h cpu6502CUDA.h cpu6502CUDA.cu $(PETSCII)
	-$(NVCC) $(NVCCFLAGS) -DENGINE_CUDA -o $@ main002.cpp cpu6502CUDA.cu $(PETSCII)

main003c.exe: main003.cpp main.h cpu6502CUDA.h cpu6502CUDA.cu
	-$(NVCC) $(NVCCFLAGS) -DENGINE_CUDA -o $@ main003.cpp cpu6502CUDA.cu

t: main001t.exe main002t.exe main003t.exe

cuda: main001c.exe main002c.exe main003c.exe

test: main001.exe main002.exe main003.exe main001t.exe main002t.exe main003t.exe
	./main001.exe
	./main002.exe
	./main003.exe
	./main004.exe 1000
	-./main005.exe || true
	./main001t.exe
	./main002t.exe
	./main003t.exe
	./main001c.exe
	./main002c.exe
	./main003c.exe

clean:
	rm -f main001.exe main002.exe main003.exe main004.exe main005.exe
	rm -f main001t.exe main002t.exe main003t.exe
	rm -f main001c.exe main002c.exe main003c.exe