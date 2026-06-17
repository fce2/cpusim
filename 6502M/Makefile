CXX  = g++
CFLAGS = -O2

.PHONY: all clean

all: main001.exe

main001.exe: main001.cpp cpu6502M.h
	$(CXX) $(CFLAGS) -o $@ main001.cpp

clean:
	rm -f main001.exe


test: main001.exe
	.\main001.exe
