#include "cpu6502.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __OSCAR64C__
	#define LOADROMS
#else
	static void load(u8 *ram, const char* path, u16 addr) {
		FILE* f = fopen(path, "rb");
		if (!f) { fprintf(stderr, "Can't open %s\n", path); exit(1); }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		fread(&ram[addr], 1, (size_t)n, f);
		fclose(f);
	}
	#define LOADROMS load(ram, "basic_a000.rom",  0xA000); load(ram, "kernal_e000.rom", 0xE000);
#endif
