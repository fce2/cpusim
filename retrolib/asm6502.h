#ifndef _ASM6502_H_
#define _ASM6502_H_

#if defined(__cplusplus) && !defined(__OSCAR64C__)
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

enum { AM_ZPG, AM_IMM, AM_ZPX, AM_ABX, AM_IZX, AM_IZY, AM_ABS, AM_IMP, AM_REL, AM_ZPY, AM_ABY };

static const char* const op_names[] =
{
	"adc","and","asl","bcc","bcs","beq","bit","bmi","bne","bpl","brk","bvc","bvs","clc","cld","cli","clv","cmp","cpx","cpy",
	"dec","dex","dey","eor","inc","inx","iny","jmp","jsr","lda","ldx","ldy","lsr","nop","ora","pha","php","pla","plp","rol",
	"ror","rti","rts","sbc","sec","sed","sei","sta","stx","sty","tax","tay","tsx","txa","txs","tya","-?-"
};

enum {
	OP_ADC = 0, OP_AND = 1, OP_ASL = 2, OP_BCC = 3, OP_BCS = 4, OP_BEQ = 5, OP_BIT = 6, OP_BMI = 7, OP_BNE = 8, OP_BPL = 9, OP_BRK = 10, OP_BVC = 11, OP_BVS = 12, OP_CLC = 13,
	OP_CLD = 14, OP_CLI = 15, OP_CLV = 16, OP_CMP = 17, OP_CPX = 18, OP_CPY = 19, OP_DEC = 20, OP_DEX = 21, OP_DEY = 22, OP_EOR = 23, OP_INC = 24, OP_INX = 25, OP_INY = 26,
	OP_JMP = 27, OP_JSR = 28, OP_LDA = 29, OP_LDX = 30, OP_LDY = 31, OP_LSR = 32, OP_NOP = 33, OP_ORA = 34, OP_PHA = 35, OP_PHP = 36, OP_PLA = 37, OP_PLP = 38, OP_ROL = 39,
	OP_ROR = 40, OP_RTI = 41, OP_RTS = 42, OP_SBC = 43, OP_SEC = 44, OP_SED = 45, OP_SEI = 46, OP_STA = 47, OP_STX = 48, OP_STY = 49, OP_TAX = 50, OP_TAY = 51, OP_TSX = 52,
	OP_TXA = 53, OP_TXS = 54, OP_TYA = 55, OP_INV = 56
};

typedef struct OP_INFO {
	unsigned char op;
	unsigned char mode;
	unsigned char len;
} OP_INFO;

static const OP_INFO ops[256] =
{
	{OP_BRK,AM_IMP,1},{OP_ORA,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ORA,AM_ZPG,2},{OP_ASL,AM_ZPG,2},{OP_INV,0,1},{OP_PHP,AM_IMP,1},{OP_ORA,AM_IMM,2},{OP_ASL,AM_IMP,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ORA,AM_ABS,3},{OP_ASL,AM_ZPG,3},{OP_INV,0,1},
	{OP_BPL,AM_REL,2},{OP_ORA,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ORA,AM_ZPX,2},{OP_ASL,AM_ZPX,2},{OP_INV,0,1},{OP_CLC,AM_IMP,1},{OP_ORA,AM_ABX,3},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ORA,AM_ZPX,3},{OP_ASL,AM_ZPX,3},{OP_INV,0,1},
	{OP_JSR,AM_ABS,3},{OP_AND,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_BIT,AM_ZPG,2},{OP_AND,AM_ZPG,2},{OP_ROL,AM_ZPG,2},{OP_INV,0,1},{OP_PLP,AM_IMP,1},{OP_AND,AM_IMM,2},{OP_ROL,AM_IMP,1},{OP_INV,0,1},{OP_BIT,AM_ABS,3},{OP_AND,AM_ABS,3},{OP_ROL,AM_ZPG,3},{OP_INV,0,1},
	{OP_BMI,AM_REL,2},{OP_AND,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_AND,AM_ZPX,2},{OP_ROL,AM_ZPX,2},{OP_INV,0,1},{OP_SEC,AM_IMP,1},{OP_AND,AM_ABX,3},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_AND,AM_ZPX,3},{OP_ROL,AM_ZPX,3},{OP_INV,0,1},
	{OP_RTI,AM_IMP,1},{OP_EOR,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_EOR,AM_ZPG,2},{OP_LSR,AM_ZPG,2},{OP_INV,0,1},{OP_PHA,AM_IMP,1},{OP_EOR,AM_IMM,2},{OP_LSR,AM_IMP,1},{OP_INV,0,1},{OP_JMP,AM_ABS,3},{OP_EOR,AM_ABS,3},{OP_LSR,AM_ZPG,3},{OP_INV,0,1},
	{OP_BVC,AM_REL,2},{OP_EOR,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_EOR,AM_ZPX,2},{OP_LSR,AM_ZPX,2},{OP_INV,0,1},{OP_CLI,AM_IMP,1},{OP_EOR,AM_ABX,3},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_EOR,AM_ZPX,3},{OP_LSR,AM_ZPX,3},{OP_INV,0,1},
	{OP_RTS,AM_IMP,1},{OP_ADC,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ADC,AM_ZPG,2},{OP_ROR,AM_ZPG,2},{OP_INV,0,1},{OP_PLA,AM_IMP,1},{OP_ADC,AM_IMM,2},{OP_ROR,AM_IMP,1},{OP_INV,0,1},{OP_JMP,AM_ABS,3},{OP_ADC,AM_ABS,3},{OP_ROR,AM_ZPG,3},{OP_INV,0,1},
	{OP_BVS,AM_REL,2},{OP_ADC,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ADC,AM_ZPX,2},{OP_ROR,AM_ZPX,2},{OP_INV,0,1},{OP_SEI,AM_IMP,1},{OP_ADC,AM_ABY,3},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_ADC,AM_ZPX,3},{OP_ROR,AM_ZPX,3},{OP_INV,0,1},
	{OP_INV,0,1},{OP_STA,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_STY,AM_ZPG,2},{OP_STA,AM_ZPG,2},{OP_STX,AM_ZPG,2},{OP_INV,0,1},{OP_DEY,AM_IMP,1},{OP_INV,0,1},{OP_TXA,AM_IMP,1},{OP_INV,0,1},{OP_STY,AM_ABS,3},{OP_STA,AM_ABS,3},{OP_STX,AM_ABS,3},{OP_INV,0,1},
	{OP_BCC,AM_REL,2},{OP_STA,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_STY,AM_ZPX,2},{OP_STA,AM_ZPX,2},{OP_STX,AM_ZPY,2},{OP_INV,0,1},{OP_TYA,AM_IMP,1},{OP_STA,AM_ABY,3},{OP_TXS,AM_IMP,1},{OP_INV,0,1},{OP_INV,0,1},{OP_STA,AM_ZPX,3},{OP_INV,0,1},{OP_INV,0,1},
	{OP_LDY,AM_IMM,2},{OP_LDA,AM_IZX,2},{OP_LDX,AM_IMM,2},{OP_INV,0,1},{OP_LDY,AM_ZPG,2},{OP_LDA,AM_ZPG,2},{OP_LDX,AM_ZPG,2},{OP_INV,0,1},{OP_TAY,AM_IMP,1},{OP_LDA,AM_IMM,2},{OP_TAX,AM_IMP,1},{OP_INV,0,1},{OP_LDY,AM_ABS,3},{OP_LDA,AM_ABS,3},{OP_LDX,AM_ABS,3},{OP_INV,0,1},
	{OP_BCS,AM_REL,2},{OP_LDA,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_LDY,AM_ZPX,2},{OP_LDA,AM_ZPX,2},{OP_LDX,AM_ZPY,2},{OP_INV,0,1},{OP_CLV,AM_IMP,1},{OP_LDA,AM_ABY,3},{OP_TSX,AM_IMP,1},{OP_INV,0,1},{OP_LDY,AM_ABX,3},{OP_LDA,AM_ABX,3},{OP_LDX,AM_ABY,3},{OP_INV,0,1},
	{OP_CPY,AM_IMM,2},{OP_CMP,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_CPY,AM_ZPG,2},{OP_CMP,AM_ZPG,2},{OP_DEC,AM_ZPG,2},{OP_INV,0,1},{OP_INY,AM_IMP,1},{OP_CMP,AM_IMM,2},{OP_DEX,AM_IMP,1},{OP_INV,0,1},{OP_CPY,AM_ABS,3},{OP_CMP,AM_ABS,3},{OP_DEC,AM_ABS,3},{OP_INV,0,1},
	{OP_BNE,AM_REL,2},{OP_CMP,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_CMP,AM_ZPX,2},{OP_DEC,AM_ZPX,2},{OP_INV,0,1},{OP_CLD,AM_IMP,1},{OP_CMP,AM_ABY,3},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_CMP,AM_ZPX,3},{OP_DEC,AM_ZPX,3},{OP_INV,0,1},
	{OP_CPX,AM_IMM,2},{OP_SBC,AM_IZX,2},{OP_INV,0,1},{OP_INV,0,1},{OP_CPX,AM_ZPG,2},{OP_SBC,AM_ZPG,2},{OP_INC,AM_ZPG,2},{OP_INV,0,1},{OP_INX,AM_IMP,1},{OP_SBC,AM_IMM,2},{OP_NOP,AM_IMP,1},{OP_INV,0,1},{OP_CPX,AM_ABS,3},{OP_SBC,AM_ABS,3},{OP_INC,AM_ABS,3},{OP_INV,0,1},
	{OP_BEQ,AM_REL,2},{OP_SBC,AM_IZY,2},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_SBC,AM_ZPX,2},{OP_INC,AM_ZPX,2},{OP_INV,0,1},{OP_SED,AM_IMP,1},{OP_SBC,AM_ABY,3},{OP_INV,0,1},{OP_INV,0,1},{OP_INV,0,1},{OP_SBC,AM_ZPX,3},{OP_INC,AM_ZPX,3},{OP_INV,0,1}
};

typedef struct { const char* pre; const char* post; } AM_FMT;

static const AM_FMT am_fmt[] = { { "$", "" },{ "#$", "" },{ "$", ",X" },{ "$", ",X" },{ "($", ",X)" },{ "($", "),Y" },{ "$", "" },{ "", "" },{ "$", "" },{ "$", ",Y" },{ "$", ",Y" } };

static const char* const am_name[] = { "zpg", "imm", "zpx", "abx", "izx", "izy", "abs", "imp", "rel", "zpy", "aby" };

static const char* opName(int opcode) { return op_names[ops[opcode].op]; }
static int opNumBytes(int opcode) { return ops[opcode].len; }
static int opMode(int opcode) { return ops[opcode].mode; }
static int opIsInvalid(int opcode) { return ops[opcode].op == OP_INV; }

static int hexdig(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'A' && c <= 'F') return c - 'A' + 10; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; }
static int hexdig2(const char* s) { int h = hexdig(s[0]), l = hexdig(s[1]); return (h < 0 || l < 0) ? -1 : (h << 4) | l; }
static int hexdig4(const char* s) { int h = hexdig2(s), l = hexdig2(s + 2); return (h < 0 || l < 0) ? -1 : (h << 8) | l; }

static uint8_t* asm6502(uint8_t* ram, uint8_t* addr, const char* txt)
{
	const char* orig = txt;
	uint16_t here = (uint16_t)(addr - ram);
	while (isspace(*txt)) txt++;
	if (!strncmp(txt, "DB", 2) || !strncmp(txt, "DW", 2)) {
		txt += 2;
		while (isspace(*txt)) txt++;
		if (*txt == '$') txt++;
		int l = orig[1] == 'B' ? hexdig2(txt) : hexdig4(txt);
		if (l >= 0) {
			*addr++ = l & 0xFF;
			if (orig[1] == 'W') *addr++ = l >> 8;
		}
		return addr;
	}
	int op;
	for (op = 0; op < 256; op++) {
		if (ops[op].op == OP_INV) continue;
		const char* t = orig;
		int oplen = ops[op].len;
		const char* name = op_names[ops[op].op];
		int mode = ops[op].mode;
		const char* pre = am_fmt[mode].pre;
		const char* post = am_fmt[mode].post;
		if (oplen == 1) { if (!strcmp(t, name)) { *addr++ = op; return addr; } }
		else {
			int l = (int)strlen(name);
			if (strncmp(t, name, l)) continue; else t += l;
			while (isspace(*t)) t++;
			l = (int)strlen(pre);
			if (strncmp(t, pre, l)) continue; else t += l;
			int tlen = (int)strlen(t);
			int postlen = (int)strlen(post);
			if (tlen >= postlen && !strcmp(t + tlen - postlen, post)) {
				if (*t == '$' && mode != AM_ZPG) t++;
				if (mode == AM_REL && tlen - postlen == 4) {
					l = hexdig4(t);
					if (l >= 0) { *addr++ = op; *addr++ = (uint8_t)(int16_t)(l - (here + 2)); return addr; }
				}
				if (oplen == 2 && tlen - postlen == 2) { l = hexdig2(t); if (l >= 0 && hexdig(*(t + 2)) < 0) { *addr++ = op; *addr++ = l; return addr; } }
				else if (oplen == 3 && tlen - postlen == 4) { l = hexdig4(t); if (l >= 0) { *addr++ = op; *addr++ = l & 0xff; *addr++ = l >> 8; return addr; } }
			}
		}
	}
	printf("could not assemble '%s'\n", orig);
	return addr;
}

typedef struct Disasm6502 {
	uint16_t addr;			/* address of instruction */
	uint8_t bytes[3];		/* raw bytes (up to 3) */
	uint8_t len;			/* instruction length (1-3) */
	uint8_t op;				/* op index into op_names */
	uint8_t mode;			/* addressing mode (AM_*) */
	char mnemonic[8];		/* e.g. "LDA" */
	char operand[24];		/* e.g. "$DD00" or "#$07" */
	uint16_t target;		/* branch/jump target, 0 if none */
} Disasm6502;

static int disasm6502_one(uint16_t addr, Disasm6502* out, const uint8_t* mem)
{
	out->addr = addr;
	out->len = 0;
	out->target = 0;
	memset(out->bytes, 0, 3);
	memset(out->mnemonic, 0, 8);
	memset(out->operand, 0, 24);
	uint8_t opcode = mem[addr];
	const OP_INFO* o = &ops[opcode];
	out->op = o->op;
	out->mode = o->mode;
	out->len = o->len;
	int i;
	for (i = 0; i < o->len && i < 3; i++) out->bytes[i] = mem[addr + i];
	if (o->op == OP_INV) { strcpy(out->mnemonic, ".byte"); snprintf(out->operand, 24, "$%02X", opcode); out->len = 1; return 1; }
	strcpy(out->mnemonic, op_names[o->op]);
	uint16_t val = 0;
	if (o->mode == AM_REL) val = addr + 2 + (int8_t)mem[addr + 1];
	else if (o->len == 2) val = mem[addr + 1];
	else if (o->len == 3) val = mem[addr + 1] | (mem[addr + 2] << 8);
	if (o->mode == AM_REL) out->target = val;
	else if (o->op == OP_JMP && o->len == 3) out->target = val;
	else if (o->op == OP_JSR) out->target = val;
	if (o->mode == AM_IMP) {}
	else if (o->mode == AM_REL) { snprintf(out->operand, 24, "$%04X", val); }
	else {
		const AM_FMT* fmt = &am_fmt[o->mode];
		if (o->len == 2) snprintf(out->operand, 24, "%s%02X%s", fmt->pre, val, fmt->post);
		else snprintf(out->operand, 24, "%s%04X%s", fmt->pre, val, fmt->post);
	}
	return out->len;
}

static void disasm6502(const uint8_t* mem, uint16_t start, uint16_t end)
{
	uint16_t pc;
	int i;
	for (pc = start; pc < end;)
	{
		Disasm6502 inst;
		int len = disasm6502_one(pc, &inst, mem);
		printf("%04X: ", pc);
		for (i = 0; i < 3; i++) printf(i < len ? "%02X " : "   ", inst.bytes[i]);
		if (inst.operand[0]) printf("%-4s %s\n", inst.mnemonic, inst.operand);
		else printf("%s\n", inst.mnemonic);
		pc += len;
	}
}

#if defined(__cplusplus) && !defined(__OSCAR64C__)
}
#endif

#endif
