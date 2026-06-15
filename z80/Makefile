ROOT = ..

CXX  = g++
CFLAGS = -O2 -I$(ROOT) -DDEBUG -DCOUNT_CYCLES -DILLEGAL

.PHONY: all clean

all: main001.exe

main001.exe: main001.cpp cpuZ80.c
	$(CXX) $(CFLAGS) -DDEBUG -DSINGLE_INST -o $@ main001.cpp cpuZ80.c

clean:
	rm -f main001.exe
