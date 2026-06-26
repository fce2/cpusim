#include <chrono>
using clk = std::chrono::high_resolution_clock;
#include "main.h"
#include "asm6502.h"

#if defined(ENGINE_CUDA)
static const int BUDGET = 1000;
#elif defined(ENGINE_T)
static const int BUDGET = 10 * 1000;
#else
static const int BUDGET = 10 * 1000 * 1000;
#endif

static u8 ram[65536] = {};

static u8* branch(u8* a, const char* mnemonic, u8* target) {
	char buf[32];
	snprintf(buf, sizeof(buf), "%s $%04X", mnemonic, (unsigned)(target - ram));
	return asm6502(ram, a, buf);
}

static u16 t_start, t_end;

static void test_idle() {
	t_start = 0x0400;
	u8* a = asm6502(ram, &ram[t_start], "jmp $0400");
	t_end = (u16)(a - ram);
}

static void test_counter() {
	t_start = 0x0300;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "ldx #$FF");
	u8* loop = a;
	a = asm6502(ram, a, "dex");
	a = branch(a, "bne", loop);
	u8* idle = a;
	char buf[32];
	snprintf(buf, sizeof(buf), "jmp $%04X", (unsigned)(idle - ram));
	a = asm6502(ram, a, buf);
	t_end = (u16)(a - ram);
}

static void test_fib20() {
	t_start = 0x0200;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "lda #$01");
	a = asm6502(ram, a, "sta $10");
	a = asm6502(ram, a, "sta $11");
	a = asm6502(ram, a, "lda #$14");
	a = asm6502(ram, a, "sta $12");
	u8* loop = a;
	a = asm6502(ram, a, "lda $11");
	a = asm6502(ram, a, "sta $13");
	a = asm6502(ram, a, "clc");
	a = asm6502(ram, a, "adc $10");
	a = asm6502(ram, a, "sta $11");
	a = asm6502(ram, a, "lda $13");
	a = asm6502(ram, a, "sta $10");
	a = asm6502(ram, a, "dec $12");
	a = branch(a, "bne", loop);
	{ u8* idle = a; char buf[32]; snprintf(buf, sizeof(buf), "jmp $%04X", (unsigned)(idle - ram)); a = asm6502(ram, a, buf); }
	t_end = (u16)(a - ram);
}

static void test_memfill() {
	t_start = 0x0500;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "ldx #$00");
	a = asm6502(ram, a, "lda #$AA");
	u8* loop = a;
	a = asm6502(ram, a, "sta $1000,X");
	a = asm6502(ram, a, "inx");
	a = branch(a, "bne", loop);
	{ u8* idle = a; char buf[32]; snprintf(buf, sizeof(buf), "jmp $%04X", (unsigned)(idle - ram)); a = asm6502(ram, a, buf); }
	t_end = (u16)(a - ram);
}

static void test_copy() {
	t_start = 0x0600;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "ldy #$FF");
	u8* loop = a;
	a = asm6502(ram, a, "lda ($10),Y");
	a = asm6502(ram, a, "sta ($20),Y");
	a = asm6502(ram, a, "dey");
	a = branch(a, "bne", loop);
	{ u8* idle = a; char buf[32]; snprintf(buf, sizeof(buf), "jmp $%04X", (unsigned)(idle - ram)); a = asm6502(ram, a, buf); }
	ram[0x10] = 0x00; ram[0x11] = 0x20;
	ram[0x20] = 0x00; ram[0x21] = 0x30;
	t_end = (u16)(a - ram);
}

static void test_multiply() {
	t_start = 0x0700;
	u8* a = &ram[t_start];
	a = asm6502(ram, a, "lda #$00");
	a = asm6502(ram, a, "sta $12");
	a = asm6502(ram, a, "sta $13");
	a = asm6502(ram, a, "ldx #$08");
	u8* loop = a;
	a = asm6502(ram, a, "lsr $10");
	u8* bcc_pos = a;
	a = asm6502(ram, a, "bcc $0000"); /* placeholder */
	a = asm6502(ram, a, "clc");
	a = asm6502(ram, a, "lda $12");
	a = asm6502(ram, a, "adc $11");
	a = asm6502(ram, a, "sta $12");
	a = asm6502(ram, a, "lda $13");
	a = asm6502(ram, a, "adc #$00");
	a = asm6502(ram, a, "sta $13");
	/* patch BCC to skip to here */
	{ int rel = (int)((a - ram) - ((bcc_pos - ram) + 2)); bcc_pos[1] = (u8)rel; }
	a = asm6502(ram, a, "asl $11");
	a = asm6502(ram, a, "dex");
	a = branch(a, "bne", loop);
	{ u8* idle = a; char buf[32]; snprintf(buf, sizeof(buf), "jmp $%04X", (unsigned)(idle - ram)); a = asm6502(ram, a, buf); }
	ram[0x10] = 0x07;
	ram[0x11] = 0x09;
	t_end = (u16)(a - ram);
}

int main() {
	printf("=== %s ===\n", ENGINE);
	printf("%-20s %12s %15s %10s\n", "Workload", "Insns", "MIPS", "Time(ms)");
	struct W { const char* name; uint16_t pc; void (*test)(); };
	W w[] = {
		{"Counter DEX+BNE", 0x0300, test_counter},
		{"Fibonacci(20)", 0x0200, test_fib20},
		{"Idle JMP-self", 0x0400, test_idle},
		{"MemFill 256B", 0x0500, test_memfill},
		{"Copy LDAiy+STAiy", 0x0600, test_copy},
		{"Multiply 8bit", 0x0700, test_multiply},
	};
	for (auto& wl : w) {
		memset(ram, 0, sizeof(ram));
		wl.test();
		CPU6502 cpu;
		#if defined(ENGINE_T) || defined(ENGINE_CUDA)
		cpu6502_init(&cpu, ram);
		WS
		#else
		cpu6502_init(&cpu, ram, NULL, NULL);
		#endif
		cpu6502_reset(&cpu);
		cpu.PC = wl.pc; cpu.halted = false;
		auto t0 = clk::now();
		int64_t insns = 0;
		while (insns < BUDGET && !cpu.halted) { cpu6502_step(&cpu); insns++; }
		auto t1 = clk::now();
		double secs = std::chrono::duration<double>(t1 - t0).count();
		printf("%-20s %12lld %15.7f %10.4f\n", wl.name, (long long)insns, insns / secs / 1e6, secs * 1000.0);
	}
	printf("\n");
	return 0;
}