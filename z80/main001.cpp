#include <cstdio>
#include <cstring>
#include "cpuZ80.h"

static u8 ram[65536];

static const u8 prog[] = {
	/* $0000: JP $8000 (reset vector) */
	0xC3, 0x00, 0x80,
	/* $8000: the program */
	0x3E, 0x42,				/* LD	A,$42 */
	0x06, 0x07,				/* LD	B,$07 */
	0x80,					/* ADD	A,B */
	0x21, 0x00, 0xC0,		/* LD	HL,$C000 */
	0x77,					/* LD	(HL),A */
	0x76,					/* HALT */
};

int main() {
	memset(ram, 0, sizeof(ram));
	memcpy(ram, prog, 3);								/* JP $8000 at $0000 */
	memcpy(ram + 0x8000, prog + 3, sizeof(prog) - 3);	/* program at $8000 */
	cpuZ80_init(ram, NULL, NULL, NULL, NULL);
	cpuZ80_reset();
	for (int i = 0; i < 8; i++) {
		printf("[%d] ", i);
		cpuZ80_dump();
		cpuZ80_step();
	}
	printf("\nRAM[$C000] = $%02X  (expect $49)\n", ram[0xC000]);
	return 0;
}
