# Makefile for 68000 CPU simulator test
# g++ -O2 -D_WIN32 -DCOUNT_CYCLES -DADDRESS_ERROR -I.. -o main001.exe main001.cpp cpu68000.c

ROOT = ..
CXX  = g++
CFLAGS = -O2 -I$(ROOT) -D_WIN32 -DCOUNT_CYCLES -DADDRESS_ERROR

.PHONY: all clean test

all: main001.exe

main001.exe: main001.cpp cpu68000.h cpu68000.c
	$(CXX) $(CFLAGS) -o $@ main001.cpp cpu68000.c

clean:
	rm -f main001.exe

test: main001.exe
	./main001.exe