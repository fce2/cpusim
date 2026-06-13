#ifdef DEBUG
	#include <stdio.h>
#endif
#include <string.h>
#include "cpu6502.h"

#define NMI_ADDR	0xFFFA	/* NMI vector: $FFFA/$FFFB */
#define RST_ADDR	0xFFFC	/* Reset vector: $FFFC/$FFFD */
#define IRQ_ADDR	0xFFFE	/* IRQ/BRK vector: $FFFE/$FFFF */
#define STACK_PAGE	0x0100	/* stack page, SP wraps within */
#define INIT_SP		0xFD	/* SP after reset */
#define PAGE_MASK	0xFF00	/* page-crossing cycle detection */
#define ADDR_MASK	0xFFFF	/* 16-bit wrap, prevents UB on 32-bit */
#define LXAA		0xEE	/* LXA,XAA are unstable, chip-dependent constant */

/* nz_table[v] → N|Z flags. NO_NZ_TABLE: branch-based SETNZ (saves 256B). */
#ifndef NO_NZ_TABLE
	static const uint8_t nz_table[256] = {
		P6502_Z,
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,
		P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,
		P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,
		P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,P6502_N,
	};
	#define SETNZ(v) do { P = (P & ~(P6502_N|P6502_Z)) | nz_table[(uint8_t)(v)]; } while(0)
#else
	#define SETNZ(v) do { uint8_t _nz = (v); P = (P & ~(P6502_N|P6502_Z)) | (_nz & P6502_N) | (_nz ? 0 : P6502_Z); } while(0)
#endif
#define SETC(v) do { P = (P & ~P6502_C) | ((v) ? P6502_C : 0); } while(0)

/* SINGLE_INST: globals, no struct pointer. MEM_IO: callbacks for I/O hardware. */
#ifdef SINGLE_INST
	static uint8_t A, X, Y, P, SP;
	static uint16_t PC;
	static uint8_t* ram;
	static BOOL halted;
	#ifdef MEM_IO
		static cpu6502_read_cb mem_read;
		static cpu6502_write_cb mem_write;
	#endif
#else
	#define A cpu->A
	#define X cpu->X
	#define Y cpu->Y
	#define P cpu->P
	#define SP cpu->SP
	#define PC cpu->PC
	#define ram cpu->ram
	#define halted cpu->halted
	#ifdef MEM_IO
		#define mem_read cpu->mem_read
		#define mem_write cpu->mem_write
	#endif
#endif

#if defined(MEM_IO) && !defined(NO_IO_MAP)
	static uint8_t io_map[256];
#endif

/* Fetch macros. uint16_t casts prevent wrap-around UB on 16-bit targets. */
#define F (ram[PC++]) /* fetch byte, advance PC, use the value */
#define F16 (PC += 2, (uint16_t)ram[(uint16_t)(PC-2)] | ((uint16_t)ram[(uint16_t)(PC-1)] << 8)) /* fetch word, advance PC by 2 */

/* Stack at $0100-$01FF. PUSH: write $0100|SP, SP--. POP: ++SP, read. */
#define PUSH(v) do { ram[STACK_PAGE | SP] = (v); SP--; } while(0)
#define POP (ram[STACK_PAGE | ++SP])

/* ZP addresses wrap to 8 bits. ADDRX/ADDRY for page-crossing cycle checks. */
#define ZP(i) ((uint8_t)(i))
#define ZPX(i) ((uint8_t)((i) + X))
#define ZPY(i) ((uint8_t)((i) + Y))
#define ADDRX(a) ((uint16_t)((a) + X))
#define ADDRY(a) ((uint16_t)((a) + Y))

/* io_map[page]=0→RAM, nonzero→callback. NO_IO_MAP: always callback (saves 256B). */
#if !defined(MEM_IO)
	#define IO_RD(a) ram[a]
	#define IO_WR(a,v) ram[a] = v
#elif defined(NO_IO_MAP)
	#define IO_RD(a) mem_read(_CPUCA a)
	#define IO_WR(a,v) mem_write(_CPUCA a, v)
#elif defined(SINGLE_INST)
	#define IO_RD(a) (io_map[(a) >> 8] ? mem_read(a) : ram[a])
	#define IO_WR(a,v) do { if (io_map[(a) >> 8]) mem_write(a, v); else ram[a] = v; } while(0)
#else
	#define IO_RD(a) (io_map[(a) >> 8] ? mem_read(_CPUCA a) : ram[a])
	#define IO_WR(a,v) do { if (io_map[(a) >> 8]) mem_write(_CPUCA a, v); else ram[a] = v; } while(0)
#endif

#ifdef SINGLE_INST
	#define RD(a)	rd(a)
	#define WR(a,v)	wr(a,v)
	#define INX(i)	inX(i)
	#define INY(i)	inY(i)
	#define INZ(i)	inZ(i)
#else
	#define RD(a)	rd(cpu,a)
	#define WR(a,v)	wr(cpu,a,v)
	#define INX(i)	inX(cpu,i)
	#define INY(i)	inY(cpu,i)
	#define INZ(i)	inZ(cpu,i)
#endif

INLINE uint8_t rd(_CPUC uint16_t a) { return IO_RD(a); }
INLINE void wr(_CPUC uint16_t a, uint8_t v) { IO_WR(a, v); }

/* (zp,X) wraps in ZP; (zp),Y and (zp) do not. */
INLINE uint16_t inX(_CPUC uint8_t i) { uint8_t p = (i + X) & 0xFF; return (uint16_t)rd(_CPUCA p) | ((uint16_t)rd(_CPUCA(p + 1) & 0xFF) << 8); }
INLINE uint16_t inY(_CPUC uint8_t i) { uint16_t p = (uint16_t)rd(_CPUCA i) | ((uint16_t)rd(_CPUCA(i + 1) & 0xFF) << 8); return p + Y; }
INLINE uint16_t inZ(_CPUC uint8_t i) { return (uint16_t)rd(_CPUCA i) | ((uint16_t)rd(_CPUCA(i + 1) & 0xFF) << 8); }

/* Page-crossing cycle penalty check. Peeks at fetched operands in RAM to avoid recomputing. */
#ifdef COUNT_CYCLES
	INLINE BOOL xInYf(uint8_t* mem, uint16_t pc, uint8_t y) { uint8_t zp = mem[(pc - 1) & ADDR_MASK]; uint16_t base = mem[zp] | ((uint16_t)mem[(zp + 1) & 0xFF] << 8); return (BOOL)((base & PAGE_MASK) != ((base + y) & PAGE_MASK)); }
	INLINE BOOL xAddrXf(uint8_t* mem, uint16_t pc, uint8_t x) { uint16_t base = mem[(pc - 2) & ADDR_MASK] | ((uint16_t)mem[(pc - 1) & ADDR_MASK] << 8); return (BOOL)((base & PAGE_MASK) != ((base + x) & PAGE_MASK)); }
	INLINE BOOL xAddrYf(uint8_t* mem, uint16_t pc, uint8_t y) { uint16_t base = mem[(pc - 2) & ADDR_MASK] | ((uint16_t)mem[(pc - 1) & ADDR_MASK] << 8); return (BOOL)((base & PAGE_MASK) != ((base + y) & PAGE_MASK)); }
	#define xInY_ xInYf(ram, PC, Y)
	#define xAddrX_ xAddrXf(ram, PC, X)
	#define xAddrY_ xAddrYf(ram, PC, Y)
#else
	#define xInY_ FALSE
	#define xAddrX_ FALSE
	#define xAddrY_ FALSE
#endif

#if defined(ROCKWELL65C02) || defined(SYNERTEK65C02) || defined(WDC65C02)
	#define CMOS
#endif

/* RICOH2A03 (NES): NMOS without BCD — decimal flag silently ignored. */
#ifdef RICOH2A03
	#define DEC_CHECK 0
#else
	#define DEC_CHECK P6502_D
#endif

/* CMOS vs NMOS: decimal NZ flags, JMP (ind) page-wrap, RMW cycles, BCD subtraction. */
#ifdef CMOS
	#define DEC_NZ(r, mid, bin) (P & ~(P6502_N|P6502_Z)) | ((r) & P6502_N) | ((r) ? 0 : P6502_Z)
	#define CLR_D P &= ~P6502_D
	#define DEC_EXTRA_CYC CYC(1)
	#define JMP_IND_HI(a) ((a) + 1) /* CMOS fixes the page-1 wrap bug */
	#define SBC_DEC_RESULT \
				Ao = A; \
				al_ = (A & 0x0F) - (val & 0x0F) - ((P & P6502_C) ? 0 : 1); \
				result_ = (int16_t)t2; \
				if (result_ < 0) result_ -= 0x60; \
				if (al_ < 0) result_ -= 0x06; \
				A = result_ & 0xFF;
	#define RMW_AX_CYC CYC(6); if (xAddrX_) CYC(1) /* CMOS: 6 or 7 cycles */
	#define JMP_IND_CYC CYC(6) /* CMOS: JMP (ind) always 6 cycles */
#else
	#define DEC_NZ(r, mid, bin) (P & ~(P6502_N|P6502_Z)) | ((mid) & P6502_N) | (((uint8_t)(bin)) ? 0 : P6502_Z)
	#define CLR_D
	#define DEC_EXTRA_CYC
	/* NMOS bug: JMP ($xxFF) reads high byte from $xx00 not $(xx+1)00. */
	#define JMP_IND_HI(a) (((a) & 0xFF) == 0xFF ? ((a) & PAGE_MASK) : ((a) + 1))
	#define SBC_DEC_RESULT \
				Ao = A; \
				lo_ = (A & 0x0F) - (val & 0x0F) - ((P & P6502_C) ? 0 : 1); \
				borrow_lo_ = lo_ < 0; \
				if (borrow_lo_) lo_ = ((lo_ - 6) & 0x0F); \
				hi_ = (A >> 4) - (val >> 4) - borrow_lo_; \
				if (hi_ < 0) hi_ = ((hi_ - 6) & 0x0F); \
				A = ((hi_ & 0x0F) << 4) | (lo_ & 0x0F);
	#define RMW_AX_CYC CYC(7) /* NMOS: always +1 for abs,X read-modify-write */
	#define JMP_IND_CYC CYC(5) /* NMOS: JMP (ind) 5 cycles */
#endif

void cpu6502_init(_CPUC uint8_t* ram_ptr, cpu6502_read_cb rd_cb, cpu6502_write_cb wr_cb) {
	ram = ram_ptr;
#ifdef MEM_IO
	mem_read = rd_cb; mem_write = wr_cb;
#ifndef NO_IO_MAP
	memset(io_map, 0, sizeof(io_map));
#endif
#else
	(void)rd_cb; (void)wr_cb;
#endif
	cpu6502_reset(_CPUPA);
}

#ifdef SINGLE_INST
static CPU6502* g_cpu;
void cpu6502_set(CPU6502* c) {
	g_cpu = c;
	A = c->A; X = c->X; Y = c->Y; P = c->P; SP = c->SP;
	PC = c->PC; halted = c->halted;
#ifdef MEM_IO
	mem_read = c->mem_read; mem_write = c->mem_write;
#endif
}
CPU6502* cpu6502_get(void) {
	CPU6502* c = g_cpu;
	c->A = A; c->X = X; c->Y = Y; c->P = P; c->SP = SP;
	c->PC = PC; c->halted = halted;
#ifdef MEM_IO
	c->mem_read = mem_read; c->mem_write = mem_write;
#endif
	return c;
}
const CPU6502* cpu6502_ptr(void) {
	return g_cpu;
}
#endif

#if defined(MEM_IO) && !defined(NO_IO_MAP)
void cpu6502_io_set(_CPUC uint8_t page, uint8_t val) {
	io_map[page] = val;
}
void cpu6502_io_range(_CPUC uint8_t lo, uint8_t hi) {
	while (lo <= hi) io_map[lo++] = 1;
}
#endif

/* Reset: PC from $FFFC/D, A=X=Y=0, P=0x20, SP=$FD. */
void cpu6502_reset(_CPUP) {
	PC = RD(RST_ADDR) | ((uint16_t)RD(RST_ADDR + 1) << 8);
	A = X = Y = 0; P = P6502_U; SP = INIT_SP; halted = 0;
}

/* Push PC+P to stack, jump to vector. CLR_D: NMOS no-op, CMOS clears decimal. */
static void int_push_jump(_CPUC uint16_t vec) {
	ram[STACK_PAGE | SP] = (PC >> 8) & 0xFF; SP--;
	ram[STACK_PAGE | SP] = PC & 0xFF; SP--;
	ram[STACK_PAGE | SP] = P | P6502_U; SP--;
	CLR_D; P |= P6502_I;
	PC = RD(vec) | ((uint16_t)RD(vec + 1) << 8);
}

void cpu6502_irq(_CPUC BOOL force) {
	if (!force && (P & P6502_I)) return;
	int_push_jump(_CPUCA IRQ_ADDR);
}

void cpu6502_nmi(_CPUP) {
	int_push_jump(_CPUCA NMI_ADDR);
}

/* F/F16 advance PC before case body so BRK/JSR read current PC. */
CYCLES cpu6502_step(_CPUP)
{
	uint16_t tmp, t2;
	uint8_t op, val, v2;
	uint8_t Ao, ci, mid;
	int16_t al, ah;
	int16_t lo_, borrow_lo_, hi_;
#ifdef ILLEGAL
	int16_t al_, result_;
	uint16_t base;
	uint8_t mask, sha_val;
	uint16_t addr;
#endif
	(void)val; (void)v2; (void)t2;
	STEPSTART;

	if (halted) {
#ifdef DEBUG
		printf("cpu6502: KIL/JAM halted at $%04X (opcode $%02X)\n", PC - 1, ram[PC - 1]);
#endif
		return STEPRET;
	}

	op = F;
	switch (op)
	{
	case 0xEA: CYC(2); break; /* NOP */
	case 0x00: PC++; PUSH((PC >> 8) & 0xFF); PUSH(PC & 0xFF); PUSH(P | P6502_U | P6502_B); CLR_D; P |= P6502_I; PC = RD(IRQ_ADDR) | ((uint16_t)RD(IRQ_ADDR + 1) << 8); CYC(7); break; /* BRK */
	case 0x40: P = (POP | P6502_U) & ~P6502_B; PC = POP; PC |= (uint16_t)POP << 8; CYC(6); break; /* RTI */
	case 0x20: { uint8_t lo = F; PUSH(PC >> 8); PUSH(PC & 0xFF); PC = lo | ((uint16_t)ram[PC] << 8); CYC(6); break; } /* JSR */
	case 0x60: PC = POP; PC |= (uint16_t)POP << 8; PC++; CYC(6); break; /* RTS */
	case 0x08: PUSH(P | P6502_U | P6502_B); CYC(3); break; /* PHP */
	case 0x28: P = POP | P6502_U; P &= ~P6502_B; CYC(4); break; /* PLP */
	case 0x48: PUSH(A); CYC(3); break; /* PHA */
	case 0x68: A = POP; SETNZ(A); CYC(4); break; /* PLA */
	case 0x8A: SETNZ(A = X); CYC(2); break; /* TXA */
	case 0x98: SETNZ(A = Y); CYC(2); break; /* TYA */
	case 0x9A: SP = X; CYC(2); break; /* TXS */
	case 0xA8: SETNZ(Y = A); CYC(2); break; /* TAY */
	case 0xAA: SETNZ(X = A); CYC(2); break; /* TAX */
	case 0xBA: SETNZ(X = SP); CYC(2); break; /* TSX */
#define BRANCH(cond) { uint8_t off = F; if (cond) { uint16_t oldPC = PC; PC += (int8_t)off; CYC(3); if ((oldPC & PAGE_MASK) != (PC & PAGE_MASK)) CYC(1); } else CYC(2); break; }
	case 0x10: BRANCH(!(P & P6502_N)) /* BPL */
	case 0x30: BRANCH(P & P6502_N) /* BMI */
	case 0x50: BRANCH(!(P & P6502_V)) /* BVC */
	case 0x70: BRANCH(P & P6502_V) /* BVS */
	case 0x90: BRANCH(!(P & P6502_C)) /* BCC */
	case 0xB0: BRANCH(P & P6502_C) /* BCS */
	case 0xD0: BRANCH(!(P & P6502_Z)) /* BNE */
	case 0xF0: BRANCH(P & P6502_Z) /* BEQ */
#define FLAG_CLR(f) P &= ~P6502_##f; CYC(2); break;
#define FLAG_SET(f) P |= P6502_##f; CYC(2); break;
	case 0x18: FLAG_CLR(C) /* CLC */
	case 0x38: FLAG_SET(C) /* SEC */
	case 0x58: FLAG_CLR(I) /* CLI */
	case 0x78: FLAG_SET(I) /* SEI */
	case 0xB8: FLAG_CLR(V) /* CLV */
	case 0xD8: FLAG_CLR(D) /* CLD */
	case 0xF8: FLAG_SET(D) /* SED */
	case 0x4C: PC = F16; CYC(3); break; /* JMP abs */
	case 0x6C: { tmp = F16; PC = (uint16_t)RD(tmp) | ((uint16_t)RD(JMP_IND_HI(tmp)) << 8); JMP_IND_CYC; break; } /* JMP (ind) */
	case 0xA1: SETNZ(A = RD(INX(F))); CYC(6); break; /* LDA (inX) */
	case 0xA5: SETNZ(A = RD(ZP(F))); CYC(3); break; /* LDA zp */
	case 0xA9: SETNZ(A = F); CYC(2); break; /* LDA imm */
	case 0xAD: SETNZ(A = RD(F16)); CYC(4); break; /* LDA abs */
	case 0xB1: SETNZ(A = RD(INY(F))); CYC(5); if (xInY_) CYC(1); break; /* LDA (inY) */
	case 0xB5: SETNZ(A = RD(ZPX(F))); CYC(4); break; /* LDA zp,X */
	case 0xB9: SETNZ(A = RD(ADDRY(F16))); CYC(4); if (xAddrY_) CYC(1); break; /* LDA abs,Y */
	case 0xBD: SETNZ(A = RD(ADDRX(F16))); CYC(4); if (xAddrX_) CYC(1); break; /* LDA abs,X */
	case 0x81: WR(INX(F), A); CYC(6); break; /* STA (inX) */
	case 0x85: WR(ZP(F), A); CYC(3); break; /* STA zp */
	case 0x8D: WR(F16, A); CYC(4); break; /* STA abs */
	case 0x91: WR(INY(F), A); CYC(6); break; /* STA (inY) */
	case 0x95: WR(ZPX(F), A); CYC(4); break; /* STA zp,X */
	case 0x99: WR(ADDRY(F16), A); CYC(5); break; /* STA abs,Y */
	case 0x9D: WR(ADDRX(F16), A); CYC(5); break; /* STA abs,X */
	case 0xA2: SETNZ(X = F); CYC(2); break; /* LDX imm */
	case 0xA6: SETNZ(X = RD(ZP(F))); CYC(3); break; /* LDX zp */
	case 0xAE: SETNZ(X = RD(F16)); CYC(4); break; /* LDX abs */
	case 0xB6: SETNZ(X = RD(ZPY(F))); CYC(4); break; /* LDX zp,Y */
	case 0xBE: SETNZ(X = RD(ADDRY(F16))); CYC(4); if (xAddrY_) CYC(1); break; /* LDX abs,Y */
	case 0x86: WR(ZP(F), X); CYC(3); break; /* STX zp */
	case 0x8E: WR(F16, X); CYC(4); break; /* STX abs */
	case 0x96: WR(ZPY(F), X); CYC(4); break; /* STX zp,Y */
	case 0xA0: SETNZ(Y = F); CYC(2); break; /* LDY imm */
	case 0xA4: SETNZ(Y = RD(ZP(F))); CYC(3); break; /* LDY zp */
	case 0xAC: SETNZ(Y = RD(F16)); CYC(4); break; /* LDY abs */
	case 0xB4: SETNZ(Y = RD(ZPX(F))); CYC(4); break; /* LDY zp,X */
	case 0xBC: SETNZ(Y = RD(ADDRX(F16))); CYC(4); if (xAddrX_) CYC(1); break; /* LDY abs,X */
	case 0x84: WR(ZP(F), Y); CYC(3); break; /* STY zp */
	case 0x8C: WR(F16, Y); CYC(4); break; /* STY abs */
	case 0x94: WR(ZPX(F), Y); CYC(4); break; /* STY zp,X */
#define LOGICAL(base, op) \
	case (base|0x01): SETNZ(A op RD(INX(F))); CYC(6); break; \
	case (base|0x05): SETNZ(A op RD(ZP(F))); CYC(3); break; \
	case (base|0x09): SETNZ(A op F); CYC(2); break; \
	case (base|0x0D): SETNZ(A op RD(F16)); CYC(4); break; \
	case (base|0x11): SETNZ(A op RD(INY(F))); CYC(5); if (xInY_) CYC(1); break; \
	case (base|0x15): SETNZ(A op RD(ZPX(F))); CYC(4); break; \
	case (base|0x19): SETNZ(A op RD(ADDRY(F16))); CYC(4); if (xAddrY_) CYC(1); break; \
	case (base|0x1D): SETNZ(A op RD(ADDRX(F16))); CYC(4); if (xAddrX_) CYC(1); break;
		LOGICAL(0x00, |=) /* ORA */
			LOGICAL(0x20, &=) /* AND */
			LOGICAL(0x40, ^=) /* EOR */
			/* BCD: NMOS NZ from binary mid-result, CMOS from final. RICOH2A03 ignores D. NMOS SBC uses nibble correction. */
#define ADC \
	if (P & DEC_CHECK) { \
		Ao = A; ci = (P & P6502_C) ? 1 : 0; \
		al = (A & 0x0F) + (val & 0x0F) + ci; \
		if (al > 9) al += 6; \
		ah = (A >> 4) + (val >> 4) + (al > 0x0F); \
		mid = (uint8_t)((ah << 4) | (al & 0x0F)); \
		if ((~(Ao ^ val) & (Ao ^ mid) & P6502_N)) P |= P6502_V; else P &= ~P6502_V; \
		SETC(ah > 9); \
		if (ah > 9) ah += 6; \
		A = ((ah & 0x0F) << 4) | (al & 0x0F); \
		P = DEC_NZ(A, mid, Ao + val + ci); DEC_EXTRA_CYC; \
	} else { \
		t2 = A + val + (P & P6502_C ? 1 : 0); \
		if ((~(A ^ val) & (A ^ t2) & P6502_N) != 0) P |= P6502_V; else P &= ~P6502_V; \
		SETC(t2 > 0xFF); \
		SETNZ(A = t2); \
	}
	case 0x61: val = RD(INX(F)); ADC; CYC(6); break; /* ADC (inX) */
	case 0x65: val = RD(ZP(F)); ADC; CYC(3); break; /* ADC zp */
	case 0x69: val = F; ADC; CYC(2); break; /* ADC imm */
	case 0x6D: val = RD(F16); ADC; CYC(4); break; /* ADC abs */
	case 0x71: val = RD(INY(F)); ADC; CYC(5); if (xInY_) CYC(1); break; /* ADC (inY) */
	case 0x75: val = RD(ZPX(F)); ADC; CYC(4); break; /* ADC zp,X */
	case 0x79: val = RD(ADDRY(F16)); ADC; CYC(4); if (xAddrY_) CYC(1); break; /* ADC abs,Y */
	case 0x7D: val = RD(ADDRX(F16)); ADC; CYC(4); if (xAddrX_) CYC(1); break; /* ADC abs,X */
#define SBC \
	t2 = A - val - ((P & P6502_C) ? 0 : 1); \
	if (P & DEC_CHECK) { \
		SBC_DEC_RESULT \
		if (((Ao ^ val) & P6502_N) && ((Ao ^ (uint8_t)t2) & P6502_N)) P |= P6502_V; else P &= ~P6502_V; \
		SETC(t2 < 0x100); \
		P = DEC_NZ(A, (uint8_t)t2, t2); DEC_EXTRA_CYC; \
	} else { \
		if (((A ^ val) & P6502_N) && ((A ^ (uint8_t)t2) & P6502_N)) P |= P6502_V; else P &= ~P6502_V; \
		SETC(t2 < 0x100); \
		SETNZ(A = t2); \
	}
	case 0xE1: val = RD(INX(F)); SBC; CYC(6); break; /* SBC (inX) */
	case 0xE5: val = RD(ZP(F)); SBC; CYC(3); break; /* SBC zp */
	case 0xE9: val = F; SBC; CYC(2); break; /* SBC imm */
#ifndef CMOS
	case 0xEB: val = F; SBC; CYC(2); break; /* USBC = SBC imm (NMOS only) */
#endif
	case 0xED: val = RD(F16); SBC; CYC(4); break; /* SBC abs */
	case 0xF1: val = RD(INY(F)); SBC; CYC(5); if (xInY_) CYC(1); break; /* SBC (inY) */
	case 0xF5: val = RD(ZPX(F)); SBC; CYC(4); break; /* SBC zp,X */
	case 0xF9: val = RD(ADDRY(F16)); SBC; CYC(4); if (xAddrY_) CYC(1); break; /* SBC abs,Y */
	case 0xFD: val = RD(ADDRX(F16)); SBC; CYC(4); if (xAddrX_) CYC(1); break; /* SBC abs,X */
#define CMP(r) val = (uint8_t)(r - tmp); SETC(r >= tmp); SETNZ(val); break;
	case 0xC1: tmp = RD(INX(F)); CYC(6); CMP(A) /* CMP (inX) */
	case 0xC5: tmp = RD(ZP(F)); CYC(3); CMP(A) /* CMP zp */
	case 0xC9: tmp = F; CYC(2); CMP(A) /* CMP imm */
	case 0xCD: tmp = RD(F16); CYC(4); CMP(A) /* CMP abs */
	case 0xD1: tmp = RD(INY(F)); CYC(5); if (xInY_) CYC(1); CMP(A) /* CMP (inY) */
	case 0xD5: tmp = RD(ZPX(F)); CYC(4); CMP(A) /* CMP zp,X */
	case 0xD9: tmp = RD(ADDRY(F16)); CYC(4); if (xAddrY_) CYC(1); CMP(A) /* CMP abs,Y */
	case 0xDD: tmp = RD(ADDRX(F16)); CYC(4); if (xAddrX_) CYC(1); CMP(A) /* CMP abs,X */
	case 0xE0: tmp = F; CYC(2); CMP(X) /* CPX imm */
	case 0xE4: tmp = RD(ZP(F)); CYC(3); CMP(X) /* CPX zp */
	case 0xEC: tmp = RD(F16); CYC(4); CMP(X) /* CPX abs */
	case 0xC0: tmp = F; CYC(2); CMP(Y) /* CPY imm */
	case 0xC4: tmp = RD(ZP(F)); CYC(3); CMP(Y) /* CPY zp */
	case 0xCC: tmp = RD(F16); CYC(4); CMP(Y) /* CPY abs */
#define ASL val = RD(tmp); SETC(val & P6502_N); val <<= 1; WR(tmp, val); SETNZ(val); break;
	case 0x0A: SETC(A & P6502_N); A <<= 1; SETNZ(A); CYC(2); break; /* ASL A */
	case 0x06: tmp = ZP(F); CYC(5); ASL /* ASL zp */
	case 0x0E: tmp = F16; CYC(6); ASL /* ASL abs */
	case 0x16: tmp = ZPX(F); CYC(6); ASL /* ASL zp,X */
	case 0x1E: tmp = ADDRX(F16); RMW_AX_CYC; ASL /* ASL abs,X */
#define LSR val = RD(tmp); SETC(val & 1); val >>= 1; WR(tmp, val); SETNZ(val); break;
	case 0x4A: SETC(A & 1); A >>= 1; SETNZ(A); CYC(2); break; /* LSR A */
	case 0x46: tmp = ZP(F); CYC(5); LSR /* LSR zp */
	case 0x4E: tmp = F16; CYC(6); LSR /* LSR abs */
	case 0x56: tmp = ZPX(F); CYC(6); LSR /* LSR zp,X */
	case 0x5E: tmp = ADDRX(F16); RMW_AX_CYC; LSR /* LSR abs,X */
#define ROL val = RD(tmp); v2=P&P6502_C; SETC(val & P6502_N); val = (val << 1) | v2; WR(tmp, val); SETNZ(val); break;
	case 0x2A: val = A & P6502_N; A = (A << 1) | (P & P6502_C); SETC(val); SETNZ(A); CYC(2); break; /* ROL A */
	case 0x26: tmp = ZP(F); CYC(5); ROL /* ROL zp */
	case 0x2E: tmp = F16; CYC(6); ROL /* ROL abs */
	case 0x36: tmp = ZPX(F); CYC(6); ROL /* ROL zp,X */
	case 0x3E: tmp = ADDRX(F16); RMW_AX_CYC; ROL /* ROL abs,X */
#define ROR val = RD(tmp); v2=P&P6502_C; SETC(val & 1); val = (val >> 1) | (v2 << 7); WR(tmp, val); SETNZ(val); break;
	case 0x6A: val = A & 1; A = (A >> 1) | ((P & P6502_C) << 7); SETC(val); SETNZ(A); CYC(2); break; /* ROR A */
	case 0x66: tmp = ZP(F); CYC(5); ROR /* ROR zp */
	case 0x6E: tmp = F16; CYC(6); ROR /* ROR abs */
	case 0x76: tmp = ZPX(F); CYC(6); ROR /* ROR zp,X */
	case 0x7E: tmp = ADDRX(F16); RMW_AX_CYC; ROR /* ROR abs,X */
#define INC_DEC(o) val = RD(tmp); val o; WR(tmp, val); SETNZ(val); break;
	case 0xE6: tmp = ZP(F); CYC(5); INC_DEC(++) /* INC zp */
	case 0xEE: tmp = F16; CYC(6); INC_DEC(++) /* INC abs */
	case 0xF6: tmp = ZPX(F); CYC(6); INC_DEC(++) /* INC zp,X */
	case 0xFE: tmp = ADDRX(F16); CYC(7); INC_DEC(++) /* INC abs,X */
	case 0xE8: SETNZ(++X); CYC(2); break; /* INX */
	case 0xC8: SETNZ(++Y); CYC(2); break; /* INY */
	case 0xC6: tmp = ZP(F); CYC(5); INC_DEC(--) /* DEC zp */
	case 0xCE: tmp = F16; CYC(6); INC_DEC(--) /* DEC abs */
	case 0xD6: tmp = ZPX(F); CYC(6); INC_DEC(--) /* DEC zp,X */
	case 0xDE: tmp = ADDRX(F16); CYC(7); INC_DEC(--) /* DEC abs,X */
	case 0xCA: SETNZ(--X); CYC(2); break; /* DEX */
	case 0x88: SETNZ(--Y); CYC(2); break; /* DEY */
#define BIT P = (P & ~(P6502_N | P6502_V | P6502_Z)) | (tmp & (P6502_N | P6502_V)) | ((tmp & A) ? 0 : P6502_Z); break;
	case 0x24: tmp = RD(ZP(F)); CYC(3); BIT /* BIT zp */
	case 0x2C: tmp = RD(F16); CYC(4); BIT /* BIT abs */
		/* CMOS: BRA, INA/DEA, PHX/PLX/PHY/PLY, BIT#, STZ, TSB/TRB, JMP (abs,X), (zp), WAI/STP/RMB/SMB/BBR/BBS. */
#ifdef CMOS
	case 0x80: BRANCH(1) /* BRA */
	case 0x1A: SETNZ(++A); CYC(2); break; /* INA */
	case 0x3A: SETNZ(--A); CYC(2); break; /* DEA */
	case 0x5A: PUSH(Y); CYC(3); break; /* PHY */
	case 0x7A: Y = POP; SETNZ(Y); CYC(4); break; /* PLY */
	case 0xDA: PUSH(X); CYC(3); break; /* PHX */
	case 0xFA: X = POP; SETNZ(X); CYC(4); break; /* PLX */
#define TSB_TRB(write_val) { val = RD(tmp); WR(tmp, write_val); P = (P & ~P6502_Z) | ((A & val) ? 0 : P6502_Z); break; }
	case 0x04: tmp = ZP(F); CYC(5); TSB_TRB(val | A) /* TSB zp */
	case 0x0C: tmp = F16; CYC(6); TSB_TRB(val | A) /* TSB abs */
	case 0x14: tmp = ZP(F); CYC(5); TSB_TRB(val & ~A) /* TRB zp */
	case 0x1C: tmp = F16; CYC(6); TSB_TRB(val & ~A) /* TRB abs */
	case 0x64: WR(ZP(F), 0); CYC(3); break; /* STZ zp */
	case 0x74: WR(ZPX(F), 0); CYC(4); break; /* STZ zp,X */
	case 0x9C: WR(F16, 0); CYC(4); break; /* STZ abs */
	case 0x9E: WR(ADDRX(F16), 0); CYC(5); break; /* STZ abs,X */
	case 0x89: P = (P & ~P6502_Z) | ((A & F) ? 0 : P6502_Z); CYC(2); break; /* BIT # */
	case 0x34: tmp = RD(ZPX(F)); CYC(4); BIT /* BIT zp,X */
	case 0x3C: tmp = RD(ADDRX(F16)); CYC(4); if (xAddrX_) CYC(1); BIT /* BIT abs,X */
	case 0x7C: { uint16_t addr = ADDRX(F16); PC = RD(addr) | ((uint16_t)RD(JMP_IND_HI(addr)) << 8); CYC(6); break; } /* JMP (abs,X) */
			 /* CMOS (zp) indirect instructions */
	case 0x12: SETNZ(A |= RD(INZ(F))); CYC(5); break; /* ORA (zp) */
	case 0x32: SETNZ(A &= RD(INZ(F))); CYC(5); break; /* AND (zp) */
	case 0x52: SETNZ(A ^= RD(INZ(F))); CYC(5); break; /* EOR (zp) */
	case 0x72: { uint16_t addr = INZ(F); val = RD(addr); ADC; CYC(5); break; } /* ADC (zp) */
	case 0x92: WR(INZ(F), A); CYC(5); break; /* STA (zp) */
	case 0xB2: SETNZ(A = RD(INZ(F))); CYC(5); break; /* LDA (zp) */
	case 0xD2: { tmp = RD(INZ(F)); CYC(5); CMP(A) } /* CMP (zp) */
	case 0xF2: { uint16_t addr = INZ(F); val = RD(addr); SBC; CYC(5); break; } /* SBC (zp) */
#if defined(WDC65C02) || defined(ROCKWELL65C02)
#define RMB_SMB { uint8_t zp = F, bit = (op >> 4) & 7, v = RD(zp); WR(zp, (op & 0x80) ? (v | (1 << bit)) : (v & ~(1 << bit))); CYC(5); break; }
#define BBR_BBS { uint8_t zp = F, off = F, bit = (op >> 4) & 7, v = RD(zp); if ((op & 0x80) ? (v & (1 << bit)) : !(v & (1 << bit))) { uint16_t oldPC = PC; PC += (int8_t)off; CYC(6); if ((oldPC & PAGE_MASK) != (PC & PAGE_MASK)) CYC(1); } else CYC(5); break; }
	case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77: RMB_SMB /* RMB0-7: clear bit in zp */
	case 0x87: case 0x97: case 0xA7: case 0xB7: case 0xC7: case 0xD7: case 0xE7: case 0xF7: RMB_SMB /* SMB0-7: set bit in zp */
	case 0x0F: case 0x1F: case 0x2F: case 0x3F: case 0x4F: case 0x5F: case 0x6F: case 0x7F: BBR_BBS /* BBR0-7: branch if bit reset */
	case 0x8F: case 0x9F: case 0xAF: case 0xBF: case 0xCF: case 0xDF: case 0xEF: case 0xFF: BBR_BBS /* BBS0-7: branch if bit set */
	case 0xCB: CYC(2); break; /* WAI */
#if defined(WDC65C02)
	case 0xDB: halted = 1; CYC(3); break; /* STP */
#endif
#else
	case 0xCB: CYC(2); break; /* NOP 1-byte */
#endif
	case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xC2: case 0xE2: F; CYC(2); break; /* NOP imm */
	case 0x44: F; CYC(3); break; /* NOP zp */
	case 0x54: case 0xD4: case 0xF4: F; CYC(4); break; /* NOP zp,X */
#if !defined(WDC65C02)
	case 0xDB: F; CYC(4); break; /* NOP zp,X (Rockwell/Synertek: STP slot) */
#endif
	case 0x5C: F16; CYC(4); break; /* NOP abs */
	case 0xDC: case 0xFC: ADDRX(F16); CYC(4); break; /* NOP abs,X */
#ifdef SYNERTEK65C02
	case 0x07: case 0x27: case 0x47: case 0x67: case 0x87: case 0xA7: case 0xC7: case 0xE7: F; CYC(3); break; /* NOP zp (RMB0/2/4/6 slot) */
	case 0x17: case 0x37: case 0x57: case 0x77: case 0x97: case 0xB7: case 0xD7: case 0xF7: F; CYC(4); break; /* NOP zp (SMB1/3/5/7 slot) */
	case 0x0F: case 0x2F: case 0x4F: case 0x6F: case 0x8F: case 0xAF: case 0xCF: case 0xEF: F; F; CYC(3); break; /* NOP rel (BBR0/2/4/6 slot) */
	case 0x1F: case 0x3F: case 0x5F: case 0x7F: case 0x9F: case 0xBF: case 0xDF: case 0xFF: F; F; CYC(4); break; /* NOP rel (BBS1/3/5/7 slot) */
#endif
	default: CYC(1); break; /* undefined: test convention is 1-cycle placeholder */
#else /* NMOS */
#ifdef ILLEGAL
	/* Illegal opcodes: KIL/JAM halt. RMW (SLO,RLA,SRE,RRA,DCP,ISC) combine RMW+ALU. LAX/SAX use A&X. ANC/XAA/LXA/SHx are unstable. */
	case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2: halted = 1; CYC(11); break; /* KIL/JAM */
	case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: CYC(2); break; /* NOP 1-byte */
	case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: F; CYC(2); break; /* NOP 2-byte imm */
	case 0x04: case 0x44: case 0x64: tmp = ZP(F); CYC(3); break; /* NOP 3-byte zp */
	case 0x0C: tmp = F16; CYC(4); break; /* NOP 3-byte abs */
	case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: tmp = ZPX(F); CYC(4); break; /* NOP 3-byte zp,X */
	case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: tmp = ADDRX(F16); CYC(4); if (xAddrX_) CYC(1); break; /* NOP 3-byte abs,X */
#define ILLEGAL_RMW(row, op) \
	case (row|0x03): tmp = INX(F); CYC(8); op; break; \
	case (row|0x07): tmp = ZP(F); CYC(5); op; break; \
	case (row|0x0F): tmp = F16; CYC(6); op; break; \
	case (row|0x13): tmp = INY(F); CYC(8); op; break; \
	case (row|0x17): tmp = ZPX(F); CYC(6); op; break; \
	case (row|0x1B): tmp = ADDRY(F16); CYC(7); op; break; \
	case (row|0x1F): tmp = ADDRX(F16); CYC(7); op; break;
#define SLO val = RD(tmp); SETC(val & P6502_N); val <<= 1; WR(tmp, val); A |= val; SETNZ(A); break;
#define RLA val = RD(tmp); v2 = P & P6502_C; SETC(val & P6502_N); val = (val << 1) | v2; WR(tmp, val); A &= val; SETNZ(A); break;
#define SRE val = RD(tmp); SETC(val & 1); val >>= 1; WR(tmp, val); A ^= val; SETNZ(A); break;
#define RRA val = RD(tmp); v2 = P & P6502_C; SETC(val & 1); val = (val >> 1) | (v2 << 7); WR(tmp, val); ADC; break;
#define DCP val = RD(tmp); val--; WR(tmp, val); SETC(A >= val); SETNZ((uint8_t)(A - val)); break;
#define ISC val = RD(tmp); val++; WR(tmp, val); SBC; break;
		ILLEGAL_RMW(0x00, SLO) /* SLO */
			ILLEGAL_RMW(0x20, RLA) /* RLA */
			ILLEGAL_RMW(0x40, SRE) /* SRE */
			ILLEGAL_RMW(0x60, RRA) /* RRA */
			ILLEGAL_RMW(0xC0, DCP) /* DCP */
			ILLEGAL_RMW(0xE0, ISC) /* ISC */
	case 0xA3: SETNZ(A = X = RD(INX(F))); CYC(6); break; /* LAX (inX) */
	case 0xA7: SETNZ(A = X = RD(ZP(F))); CYC(3); break; /* LAX (zp) */
	case 0xAB: { uint8_t imm_ab = F; SETNZ(A = X = (A | LXAA) & imm_ab); CYC(2); break; } /* LXA */
	case 0xAF: SETNZ(A = X = RD(F16)); CYC(4); break; /* LAX (abs) */
	case 0xB3: SETNZ(A = X = RD(INY(F))); CYC(5); if (xInY_) CYC(1); break; /* LAX (inY) */
	case 0xB7: SETNZ(A = X = RD(ZPY(F))); CYC(4); break; /* LAX (zpY) */
	case 0xBF: SETNZ(A = X = RD(ADDRY(F16))); CYC(4); if (xAddrY_) CYC(1); break; /* LAX (absY) */
	case 0x83: WR(INX(F), A & X); CYC(6); break; /* SAX (inX) */
	case 0x87: WR(ZP(F), A & X); CYC(3); break; /* SAX (zp) */
	case 0x8F: WR(F16, A & X); CYC(4); break; /* SAX (abs) */
	case 0x97: WR(ZPY(F), A & X); CYC(4); break; /* SAX (zpY) */
	case 0xCB: { uint8_t imm = F; t2 = (A & X) - imm; SETC((A & X) >= imm); SETNZ(X = (uint8_t)t2); CYC(2); break; } /* AXS */
	case 0x0B: A &= F; SETNZ(A); SETC(A & P6502_N); CYC(2); break; /* ANC */
	case 0x2B: A &= F; SETNZ(A); SETC(A & P6502_N); CYC(2); break; /* ANC */
	case 0x4B: A &= F; SETC(A & 1); A >>= 1; SETNZ(A); CYC(2); break; /* ALR */
	case 0x6B: {
		uint8_t imm_6b = F; uint8_t t_6b = A & imm_6b; uint8_t old_C = (P & P6502_C) ? 1 : 0;
		val = (t_6b >> 1) | (old_C << 7); /* binary ARR result */
		SETNZ(val);
		P = (P & ~P6502_V) | (((val ^ (val >> 1)) & 0x20) ? P6502_V : 0);
		if (P & DEC_CHECK) { /* decimal adjustment uses pre-ROR AND result */
			uint8_t adj = val;
			if ((t_6b & 0x0F) > 4) adj = (adj & 0xF0) | ((adj + 6) & 0x0F);
			if ((t_6b & 0xF0) > 0x40) { adj = (adj + 0x60) & 0xFF; P |= P6502_C; }
			else P &= ~P6502_C;
			A = adj;
		}
		else { A = val; SETC(val & 0x40); }
		CYC(2); break;
	}
	case 0x8B: { uint8_t imm_8b = F; SETNZ(A = (A | LXAA) & X & imm_8b); CYC(2); break; } /* XAA */
	case 0xBB: { uint16_t a = ADDRY(F16); uint8_t v = RD(a); SETNZ(A = X = SP = v & SP); CYC(4); if (xAddrY_) CYC(1); break; } /* LAS */
#define SHA_STORE(addr_expr, val_expr, idx, cyc) { 	base = addr_expr; mask = ((base >> 8) + 1) & 0xFF; 	sha_val = (val_expr) & mask; 	if (((base & 0xFF) + idx) > 0xFF) addr = (sha_val << 8) | ((base + idx) & 0xFF); 	else addr = base + idx; 	WR(addr, sha_val); cyc; break; }
	case 0x93: SHA_STORE(INZ(F), A & X, Y, CYC(6)) /* SHA (inY) */
	case 0x9F: SHA_STORE(F16, A & X, Y, CYC(5)) /* SHA (absY) */
	case 0x9B: SP = A & X; SHA_STORE(F16, A & X, Y, CYC(5)) /* SHS (absY) */
	case 0x9E: SHA_STORE(F16, X, Y, CYC(5)) /* SHX (absY) */
	case 0x9C: SHA_STORE(F16, Y, X, CYC(5)) /* SHY (absX) */
#endif
#ifdef DEBUG
	default:
		printf("Illegal opcode $%02X at $%04X\n", op, PC - 1);
		PC--;
		halted = 1;
		break;
#else
	default: halted = 1; break;
#endif
#endif
	}

	return STEPRET;
}

#ifdef DEBUG
#ifdef __OSCAR64C__
void cpu6502_dump(_CCPUP) { printf("%04X: A%02X X%02X Y%02X P%02X:%c%c%c%c%c%c%c%c SP:%02X\n", PC, A, X, Y, P, P & P6502_N ? 'N' : 'n', P & P6502_V ? 'V' : 'v', P & P6502_U ? 'U' : 'u', P & P6502_B ? 'B' : 'b', P & P6502_D ? 'D' : 'd', P & P6502_I ? 'I' : 'i', P & P6502_Z ? 'Z' : 'z', P & P6502_C ? 'C' : 'c', SP); }
#else
void cpu6502_dump(_CCPUP) { printf("PC=%04X A=%02X X=%02X Y=%02X P=%02X:%c%c%c%c%c%c%c%c SP=%02X\n", PC, A, X, Y, P, P & P6502_N ? 'N' : 'n', P & P6502_V ? 'V' : 'v', P & P6502_U ? 'U' : 'u', P & P6502_B ? 'B' : 'b', P & P6502_D ? 'D' : 'd', P & P6502_I ? 'I' : 'i', P & P6502_Z ? 'Z' : 'z', P & P6502_C ? 'C' : 'c', SP); }
#endif
#endif

BOOL cpu6502_is_halted(_CPUP) { return halted; }
