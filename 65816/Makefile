# Makefile for 65816 (WDC 65C816) CPU simulator test
# g++ -O2 -DDEBUG -DCOUNT_CYCLES -I.. -o main001.exe main001.cpp cpu65816.c

ROOT = ..
CXX  = g++
CFLAGS = -O2 -I$(ROOT) -DDEBUG -DCOUNT_CYCLES

.PHONY: all clean

all: main001.exe

main001.exe: main001.cpp cpu65816.h cpu65816.c
	$(CXX) $(CFLAGS) -o $@ main001.cpp cpu65816.c

clean:
	rm -f main001.exe

test: main001.exe
	.\main001.exe
