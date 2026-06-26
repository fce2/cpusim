#include "cpu68000.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u8 ram[16*1024*1024] = {};

static void w16(u32 addr, u16 val) { ram[addr & 0xFFFFFF] = (u8)(val >> 8); ram[(addr + 1) & 0xFFFFFF] = (u8)(val & 0xFF); }
static void w32(u32 addr, u32 val) { w16(addr, (u16)(val >> 16)); w16(addr + 2, (u16)(val & 0xFFFF)); }

static CPU68000 cpu;

static void init() {
	memset(ram, 0, sizeof(ram));
	cpu68000_init(&cpu, ram, NULL, NULL, NULL);
}

/* Test 001: NOP — PC advances by 2 per step, registers unchanged */
static int test_nop() {
	init();
	w32(0, 0x00001000);         /* Initial SSP */
	w32(4, 0x00000200);         /* Initial PC (Reset vector) */
	w16(0x0200, 0x4E71);        /* NOP */
	w16(0x0202, 0x4E71);        /* NOP */
	w16(0x0204, 0x4E71);        /* NOP */
	cpu68000_reset(&cpu);
	u32 ssp0 = cpu.A[7], pc0 = cpu.PC;
	for (int i = 0; i < 3; i++) cpu68000_step(&cpu);
	int pass = 1;
	if (cpu.PC != pc0 + 6)    { printf("FAIL NOP: PC=$%06X expected $%06X\n", cpu.PC, pc0 + 6); pass = 0; }
	if (cpu.A[7] != ssp0)     { printf("FAIL NOP: SSP changed\n"); pass = 0; }
	printf(pass ? "PASS NOP\n" : "FAIL NOP\n");
	return pass;
}

/* Test 002: ADD — MOVE.L imm, MOVE.L reg, ADD.L, verify result */
static int test_add() {
	init();
	w32(0, 0x00001000);                                /* Initial SSP (Supervisor Stack Pointer) */
	w32(4, 0x00000200);                                /* Initial PC (Reset vector) */
	w16(0x0200, 0x203C); w32(0x0202, 0x12345678);    /* MOVE.L #$12345678,D0 */
	w16(0x0206, 0x2200);                            /* MOVE.L D0,D1 */
	w16(0x0208, 0xD081);                            /* ADD.L D1,D0 */
	w16(0x020A, 0x4E71);                            /* NOP */
	w16(0x020C, 0x4E75);                            /* RTS */
	cpu68000_reset(&cpu);
	for (int i = 0; i < 4; i++) cpu68000_step(&cpu);
	int pass = 1;
	if (cpu.D[0] != 0x12345678+0x12345678) { printf("FAIL ADD: D0=$%08X expected $2468ACF0\n", cpu.D[0]); pass = 0; }
	if (cpu.D[1] != 0x12345678) { printf("FAIL ADD: D1=$%08X expected $12345678\n", cpu.D[1]); pass = 0; }
	printf(pass ? "PASS ADD\n" : "FAIL ADD\n");
	return pass;
}

/* Test 003: LINK/UNLK (leave) — stack frame created then destroyed, A6 and A7 restored */
static int test_leave() {
	init();
	w32(0, 0x00001000);         /* Initial SSP */
	w32(4, 0x00000200);         /* Initial PC */
	w16(0x0200, 0x2C7C); w32(0x0202, 0xDEADBEEF);  /* MOVEA.L #$DEADBEEF,A6 */
	w16(0x0206, 0x4E56); w16(0x0208, 0xFFF8);        /* LINK A6,#-8 */
	w16(0x020A, 0x4E5E);                               /* UNLK A6 */
	cpu68000_reset(&cpu);
	for (int i = 0; i < 3; i++) cpu68000_step(&cpu);
	int pass = 1;
	if (cpu.A[6] != 0xDEADBEEF) { printf("FAIL LEAVE: A6=$%08X expected $DEADBEEF\n", cpu.A[6]); pass = 0; }
	if (cpu.A[7] != 0x1000)      { printf("FAIL LEAVE: SSP=$%08X expected $1000\n", cpu.A[7]); pass = 0; }
	printf(pass ? "PASS LEAVE\n" : "FAIL LEAVE\n");
	return pass;
}

/* Test 004: Fibonacci(25) — D0=fib(25)=75025, D1=fib(26)=121393 */
static int test_fib25() {
	init();
	w32(0, 0x00001000);         /* Initial SSP */
	w32(4, 0x00000200);         /* Initial PC */
	w16(0x0200, 0x203C); w32(0x0202, 0x00000000);  /* MOVE.L #0,D0 */
	w16(0x0206, 0x223C); w32(0x0208, 0x00000001);  /* MOVE.L #1,D1 */
	w16(0x020C, 0x243C); w32(0x020E, 0x00000019);  /* MOVE.L #25,D2 */
	/* loop: */
	w16(0x0212, 0x2601);        /* MOVE.L D1,D3 */
	w16(0x0214, 0xD280);        /* ADD.L D0,D1 */
	w16(0x0216, 0x2003);        /* MOVE.L D3,D0 */
	w16(0x0218, 0x5382);        /* SUBQ.L #1,D2 */
	w16(0x021A, 0x66F6);        /* BNE.S loop ($0212) */
	w16(0x021C, 0x4E75);        /* RTS */
	cpu68000_reset(&cpu);
	/* 3 init + 25*5 = 128 steps, use 150 to be safe */
	for (int i = 0; i < 150; i++) cpu68000_step(&cpu);
	int pass = 1;
	if (cpu.D[0] != 75025) { printf("FAIL FIB25: D0=$%08X expected $%08X\n", cpu.D[0], 75025); pass = 0; }
	if (cpu.D[1] != 121393) { printf("FAIL FIB25: D1=$%08X expected $%08X\n", cpu.D[1], 121393); pass = 0; }
	printf(pass ? "PASS FIB25\n" : "FAIL FIB25\n");
	return pass;
}

int main() {
	int pass = 1;
	pass &= test_nop();
	pass &= test_add();
	pass &= test_leave();
	pass &= test_fib25();
	printf(pass ? "\nALL PASS\n" : "\nSOME FAILED\n");
	return pass ? 0 : 1;
}
