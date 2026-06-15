LIBS = ../libs
RETROLIB = ../retrolib

ROOT = ..
CXX  = g++
OSCAR64 = oscar64.exe
CFLAGS = -O2 -I$(ROOT) -I$(LIBS) -I$(RETROLIB) -DDEBUG -DCOUNT_CYCLES -DILLEGAL
PETSCII = $(RETROLIB)/cbm_petscii.c

.PHONY: all clean

all: main000.prg main001.exe main001.prg main002.exe main003.exe

main001.exe: main001.cpp main.h cpu6502.h cpu6502.c
	$(CXX) $(CFLAGS) -DSINGLE_INST -o $@ main001.cpp cpu6502.c

main002.exe: main002.cpp main.h cpu6502.h cpu6502.c
	$(CXX) $(CFLAGS) -o $@ main002.cpp cpu6502.c

main003.exe: main003.cpp main.h cpu6502.h cpu6502.c $(PETSCII)
	$(CXX) $(CFLAGS) -DMEM_IO -o $@ main003.cpp cpu6502.c $(PETSCII)

main000.prg: main000.cpp main.h cpu6502.h cpu6502.c
	$(OSCAR64) -Os -DSINGLE_INST -DRICOH2A03 -i=$(ROOT) -o=$@ main000.cpp cpu6502.c

main001.prg: main001.cpp main.h cpu6502.h cpu6502.c
	$(OSCAR64) -O2 -DDEBUG -DSINGLE_INST -DRICOH2A03 -i=$(ROOT) -o=$@ main001.cpp cpu6502.c

clean:
	rm -f main000.exe main001.exe main002.exe main003.exe
	rm -f main000.prg main000.asm main000.int main000.lbl main000.map
	rm -f main001.prg main001.asm main001.int main001.lbl main001.map
