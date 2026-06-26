#ifdef DEBUG
	#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h>
#ifdef MEM_IO
	#include <setjmp.h>
#endif
#include "cpu68000.h"

/* Prefetch invalidation, fault struct, constants */
/* Prefetch queue invalidation: flushes IRC/IRD on branches/exceptions. Define... */
#ifdef PREFETCH
	#define PF_INVALIDATE() do { cpu->irc = 0; cpu->ird = 0; } while(0)
#else
	#define PF_INVALIDATE() ((void)0)
#endif

struct CPU68000Fault {
#ifdef MEM_IO
	jmp_buf err_jmp;
#endif
	u32 fault_addr;
	int check_align;
	int fault_is_read;
	int fault_is_program;
	u32 instr_addr;
	u32 data_fault_pc;
	u16 data_fault_ir;
	u32 prev_sp;
	u16 prev_opcode;
	int ea_pending_dec_reg;
	int ea_pending_dec_val;
	int dbcc_pending_reg;
	u32 dbcc_pending_val;
	u32 a7_committed;
	int addr_error_fired;
};

#define SR_VALID (SR_T | SR_S | SR_IM2 | SR_IM1 | SR_IM0 | SR_X | SR_N | SR_Z | SR_V | SR_C)
#define ADDR_MASK 0xFFFFFF
#define OP_DIR 0x0100
#define CCR_MASK (SR_X | SR_N | SR_Z | SR_V | SR_C)
#define SCC_TRUE 0xFF
#define SCC_FALSE 0x00
#define SHIFT_CNT_MASK 63
#define IMM_ZERO_VAL 8
#define VEC_RESET_SSP 0
#define VEC_RESET_PC 1
#define VEC_ADDRERR 3
#define VEC_ILLEGAL 4
#define VEC_DIV0 5
#define VEC_CHK 6
#define VEC_TRAPV 7
#define VEC_PRIV 8
#define VEC_LINEA 10
#define VEC_LINEF 11
#define VEC_AUTO_BASE 24
#define VEC_TRAP_BASE 32
#define SSW_RW_BIT 0x10
#define SSW_IR_MASK 0xFFE0

/* SINGLE_INST: globals vs struct aliases */
#ifdef SINGLE_INST
	static u32 D[8], A[8], USP, PC;
	static u16 SR;
	static u32 flag_n, flag_z, flag_v, flag_c, flag_x;
	static u8* ram;
	static cpu68000_read_cb mem_read;
	static cpu68000_write_cb mem_write;
	static cpu68000_intack_cb int_ack;
	static CPU68000Fault fault;
	#ifdef MEM_IO
		#define err_jmp fault.err_jmp
	#endif
	#define fault_addr fault.fault_addr
	#define check_align fault.check_align
	#define fault_is_read fault.fault_is_read
	#define fault_is_program fault.fault_is_program
	#define instr_addr fault.instr_addr
	#define data_fault_pc fault.data_fault_pc
	#define data_fault_ir fault.data_fault_ir
	#define prev_sp fault.prev_sp
	#define prev_opcode fault.prev_opcode
	#define ea_pending_dec_reg fault.ea_pending_dec_reg
	#define ea_pending_dec_val fault.ea_pending_dec_val
	#define dbcc_pending_reg fault.dbcc_pending_reg
	#define dbcc_pending_val fault.dbcc_pending_val
	#define a7_committed fault.a7_committed
	#define addr_error_fired fault.addr_error_fired
	#ifdef COUNT_CYCLES
		static int cycles;
	#endif
	#ifdef BUS_HOOKS
		#ifdef COUNT_CYCLES
			#undef STEPSTART
			#define STEPSTART cycles = 0; g_cpu.bus_cycle = 0;
		#else
			#undef STEPSTART
			#define STEPSTART g_cpu.bus_cycle = 0;
		#endif
	#endif
	static CPU68000 g_cpu;
#else
	#define D cpu->D
	#define A cpu->A
	#define USP cpu->USP
	#define PC cpu->PC
	#define SR cpu->SR
	#define flag_n cpu->flag_n
	#define flag_z cpu->flag_z
	#define flag_v cpu->flag_v
	#define flag_c cpu->flag_c
	#define flag_x cpu->flag_x
	#define ram cpu->ram
	#define mem_read cpu->mem_read
	#define mem_write cpu->mem_write
	#define int_ack cpu->int_ack
	#ifdef MEM_IO
		#define err_jmp cpu->fault->err_jmp
	#endif
	#define fault_addr cpu->fault->fault_addr
	#define check_align cpu->fault->check_align
	#define fault_is_read cpu->fault->fault_is_read
	#define fault_is_program cpu->fault->fault_is_program
	#define instr_addr cpu->fault->instr_addr
	#define data_fault_pc cpu->fault->data_fault_pc
	#define data_fault_ir cpu->fault->data_fault_ir
	#define prev_sp cpu->fault->prev_sp
	#define prev_opcode cpu->fault->prev_opcode
	#define ea_pending_dec_reg cpu->fault->ea_pending_dec_reg
	#define ea_pending_dec_val cpu->fault->ea_pending_dec_val
	#define dbcc_pending_reg cpu->fault->dbcc_pending_reg
	#define dbcc_pending_val cpu->fault->dbcc_pending_val
	#define a7_committed cpu->fault->a7_committed
	#define addr_error_fired cpu->fault->addr_error_fired
	#ifdef COUNT_CYCLES
		#undef STEPSTART
		#undef CYC
		#undef STEPRET
		#ifdef BUS_HOOKS
			#define STEPSTART cpu->cycles = 0; bus_cycle = 0;
		#else
			#define STEPSTART cpu->cycles = 0;
		#endif
		#define CYC(n) cpu->cycles += n
		#define STEPRET cpu->cycles
	#else
		#ifdef BUS_HOOKS
			#undef STEPSTART
			#define STEPSTART bus_cycle = 0;
		#endif
	#endif
	#ifdef BUS_HOOKS
		#define bus_cycle cpu->bus_cycle
		#define on_bus cpu->on_bus
	#endif
#endif

/* EA cycle tables & inline cycle helpers */
/* EA cycle tables: data access (word/long) and address-calculation only. PRM ... */
static const u8 ea_cyc_w[12] = {
/* Dn An (An)(An)+-(An)d16 d8X absW absL #imm */
	0, 0, 4, 4, 6, 8, 10, 8, 12, 0, 0, 0
};
static const u8 ea_cyc_l[12] = {
	0, 0, 8, 8, 10, 12, 14, 12, 16, 0, 0, 0
};
static const u8 ea_calc_cyc[12] = {
/* Dn An (An)(An)+-(An)d16 d8X absW absL #imm */
	0, 0, 0, 0, 0, 4, 6, 4, 8, 0, 0, 0
};
/* Immediate EA cycles */
INLINE int ea_cyc(int mode, int sz) {
	return (sz == 4) ? ea_cyc_l[mode] : ea_cyc_w[mode];
}
/* EA cycles for mode 7 sub-registers */
INLINE int ea_cyc7(int reg, int sz) {
	if (reg == 0) return (sz == 4) ? 12 : 8; /* abs.W */
	if (reg == 1) return (sz == 4) ? 16 : 12; /* abs.L */
	return (sz == 4) ? 8 : 4; /* #imm (reg 4) and others */
}
INLINE int calc_cyc(int mode) { return ea_calc_cyc[mode]; }
/* ALU cycles: base + EA. .B/.W reg-reg=4, .L reg-reg=8 (6+2 footnote) */
INLINE int alu_cyc(int sz, int is_dir, int mode) {
	if (!is_dir) {
		/* EA->Dn: base+EA, .L footnote when src=Dn/#imm */
		if (sz == 4) return (mode == 0 || mode == 7) ? 8 : 6;
		return 4;
	} else {
		/* Dn->EA: read-modify-write, base+EA */
		int base = (sz == 4) ? 12 : 8;
		return base + ea_cyc(mode, sz);
	}
}
/* CMP cycles: ALU base, no dest EA */
INLINE int cmp_cyc(int sz, int sm) {
	int base = (sz == 4) ? 6 : 4;
	int ea = ea_cyc(sm, sz);
	if (sz == 4 && sm == 0) base += 2;
	return base + ea;
}

static int intack_default(_CPUC int level) {
	(void)level;
	return VEC_AUTO_BASE + level;
}

#ifdef MEM_IO

/* MEM_IO defaults */
INLINE u8 default_rd(_CPUC u32 a) { (void)cpu; return ram[a & ADDR_MASK]; }
INLINE void default_wr(_CPUC u32 a, u8 v) { (void)cpu; ram[a & ADDR_MASK] = v; }
#endif

/* SR access helpers */
u16 get_sr(_CCPUP) {
	u16 sr = SR & (SR_VALID & ~CCR_MASK);
	if (flag_x) sr |= SR_X;
	if (flag_n) sr |= SR_N;
	if (flag_z == 0) sr |= SR_Z;
	if (flag_v) sr |= SR_V;
	if (flag_c) sr |= SR_C;
	return sr;
}

void set_sr_flags(_CPUC u16 sr) {
	flag_x = (sr & SR_X) ? 0x10 : 0;
	flag_n = (sr & SR_N) ? 0x80 : 0;
	flag_z = (sr & SR_Z) ? 0 : 1;
	flag_v = (sr & SR_V) ? 0x80 : 0;
	flag_c = (sr & SR_C) ? 0x100 : 0;
}

static void write_sr(_CPUC u16 val) {
	u16 old_s = SR & SR_S;
	SR = val & SR_VALID;
	set_sr_flags(_CPUCA val);
	u16 new_s = SR & SR_S;
	if (old_s && !new_s) { u32 tsp = A[7]; A[7] = USP; USP = tsp; }
	else if (!old_s && new_s) { u32 tsp = USP; USP = A[7]; A[7] = tsp; }
}
INLINE int supervisor(_CPUP) { return (SR & SR_S) != 0; }
INLINE int popcount16(u16 v) { int c = 0; while (v) { c += v & 1; v >>= 1; } return c; }

/* Memory access */
/* RAM byte-assembly macros */
#define RAM_RD16(a) ((u16)ram[a] << 8 | ram[(a)+1])
#define RAM_RD32(a) ((u32)ram[a] << 24 | (u32)ram[(a)+1] << 16 | (u32)ram[(a)+2] << 8 | ram[(a)+3])
#define RAM_WR16(a,v) do { ram[a] = (v) >> 8; ram[(a)+1] = (v) & 0xFF; } while(0)
#define RAM_WR32(a,v) do { ram[a] = (v) >> 24; ram[(a)+1] = ((v) >> 16) & 0xFF; ram[(a)+2] = ((v) >> 8) & 0xFF; ram[(a)+3] = (v) & 0xFF; } while(0)

#ifdef FAST_MEM

INLINE u8 rd8(_CPUC u32 a) { return ram[a & ADDR_MASK]; }
INLINE u16 rd16(_CPUC u32 a) {
	if (UNLIKELY(addr_error_fired)) return 0;
	if (UNLIKELY((a & ADDR_MASK) & 1)) { fault_addr = a; fault_is_read = 1; addr_error_fired = 1; cpu->ae_fired = 1; return 0; }
	return RAM_RD16(a & ADDR_MASK);
}
INLINE u32 rd32(_CPUC u32 a) {
	if (UNLIKELY(addr_error_fired)) return 0;
	if (UNLIKELY((a & ADDR_MASK) & 1)) { fault_addr = a; fault_is_read = 1; addr_error_fired = 1; cpu->ae_fired = 1; return 0; }
	return RAM_RD32(a & ADDR_MASK);
}
INLINE void wr8(_CPUC u32 a, u8 v) { ram[a & ADDR_MASK] = v; }
INLINE void wr16(_CPUC u32 a, u16 v) {
	if (UNLIKELY(addr_error_fired)) return;
	if (UNLIKELY((a & ADDR_MASK) & 1)) { fault_addr = a; fault_is_read = 0; addr_error_fired = 1; cpu->ae_fired = 1; return; }
	RAM_WR16(a & ADDR_MASK, v);
}
INLINE void wr32(_CPUC u32 a, u32 v) {
	if (UNLIKELY(addr_error_fired)) return;
	if (UNLIKELY((a & ADDR_MASK) & 1)) { fault_addr = a; fault_is_read = 0; addr_error_fired = 1; cpu->ae_fired = 1; return; }
	RAM_WR32(a & ADDR_MASK, v);
}

#elif defined(MEM_IO)

#define ALIGN_FAULT(addr, rd) do { if (UNLIKELY(check_align && ((addr & ADDR_MASK) & 1))) { fault_addr = addr; fault_is_read = rd; longjmp(err_jmp, 1); } } while(0)
INLINE u8 rd8(_CPUC u32 a) { a &= ADDR_MASK; return mem_read(_CPUCA a); }
INLINE u16 rd16(_CPUC u32 a) {
	ALIGN_FAULT(a, 1); a &= ADDR_MASK;
	return (u16)mem_read(_CPUCA a) << 8 | mem_read(_CPUCA a + 1);
}
INLINE u32 rd32(_CPUC u32 a) {
	ALIGN_FAULT(a, 1); a &= ADDR_MASK;
	return (u32)mem_read(_CPUCA a) << 24 | (u32)mem_read(_CPUCA a + 1) << 16
		| (u32)mem_read(_CPUCA a + 2) << 8 | mem_read(_CPUCA a + 3);
}
INLINE void wr8(_CPUC u32 a, u8 v) { a &= ADDR_MASK; mem_write(_CPUCA a, v); }
INLINE void wr16(_CPUC u32 a, u16 v) {
	ALIGN_FAULT(a, 0); a &= ADDR_MASK;
	mem_write(_CPUCA a, v >> 8); mem_write(_CPUCA a + 1, v & 0xFF);
}
INLINE void wr32(_CPUC u32 a, u32 v) {
	ALIGN_FAULT(a, 0); a &= ADDR_MASK;
	mem_write(_CPUCA a, v >> 24); mem_write(_CPUCA a + 1, (v >> 16) & 0xFF);
	mem_write(_CPUCA a + 2, (v >> 8) & 0xFF); mem_write(_CPUCA a + 3, v & 0xFF);
}

#else

/* Flag-based address error recovery */
#define ALIGN_FAULT(addr, rd) do { if (UNLIKELY(check_align && ((addr & ADDR_MASK) & 1))) { fault_addr = addr; fault_is_read = rd; addr_error_fired = 1; cpu->ae_fired = 1; check_align = 0; } } while(0)
INLINE u8 rd8(_CPUC u32 a) { return ram[a & ADDR_MASK]; }
INLINE u16 rd16(_CPUC u32 a) {
	ALIGN_FAULT(a, 1);
	if (UNLIKELY(addr_error_fired)) return 0;
	return RAM_RD16(a & ADDR_MASK);
}
INLINE u32 rd32(_CPUC u32 a) {
	ALIGN_FAULT(a, 1);
	if (UNLIKELY(addr_error_fired)) return 0;
	return RAM_RD32(a & ADDR_MASK);
}
INLINE void wr8(_CPUC u32 a, u8 v) { ram[a & ADDR_MASK] = v; }
INLINE void wr16(_CPUC u32 a, u16 v) {
	ALIGN_FAULT(a, 0);
	if (UNLIKELY(addr_error_fired)) return;
	RAM_WR16(a & ADDR_MASK, v);
}
INLINE void wr32(_CPUC u32 a, u32 v) {
	ALIGN_FAULT(a, 0);
	if (UNLIKELY(addr_error_fired)) return;
	RAM_WR32(a & ADDR_MASK, v);
}

#endif

#ifdef BUS_HOOKS
#define BHV_FC_DATA ((SR & SR_S) ? FC_SUPER_DATA : FC_USER_DATA)
#define BHV_FC_PROG ((SR & SR_S) ? FC_SUPER_PROG : FC_USER_PROG)
#define BHV_CYC(sz) ((sz) == 4 ? 8 : 4)
#define BHV_EMIT(op, a, v, sz) do { if (on_bus) { cpu->fc = BHV_FC_DATA; bus_event_t e = {(op), (a), (v), cpu->fc, (sz), bus_cycle}; on_bus(_CPUCA &e); bus_cycle += BHV_CYC(sz); } } while(0)
INLINE u8 bhv_rd8(_CPUC u32 a) { u8 v = rd8(_CPUCA a); BHV_EMIT(BUS_RD, a, v, 1); return v; }
INLINE u16 bhv_rd16(_CPUC u32 a) { u16 v = rd16(_CPUCA a); BHV_EMIT(BUS_RD, a, v, 2); return v; }
INLINE u32 bhv_rd32(_CPUC u32 a) { u32 v = rd32(_CPUCA a); BHV_EMIT(BUS_RD, a, v, 4); return v; }
INLINE void bhv_wr8(_CPUC u32 a, u8 v) { wr8(_CPUCA a, v); BHV_EMIT(BUS_WR, a, v, 1); }
INLINE void bhv_wr16(_CPUC u32 a, u16 v) { wr16(_CPUCA a, v); BHV_EMIT(BUS_WR, a, v, 2); }
INLINE void bhv_wr32(_CPUC u32 a, u32 v) { wr32(_CPUCA a, v); BHV_EMIT(BUS_WR, a, v, 4); }
#undef wr8
#undef wr16
#undef wr32
#undef rd8
#undef rd16
#undef rd32
#define wr8 bhv_wr8
#define wr16 bhv_wr16
#define wr32 bhv_wr32
#define rd8 bhv_rd8
#define rd16 bhv_rd16
#define rd32 bhv_rd32
INLINE u16 bhv_ifetch(_CPUC u32 a) {
	u16 v = (u16)ram[a & ADDR_MASK] << 8 | ram[(a+1) & ADDR_MASK];
	if (on_bus) { cpu->fc = BHV_FC_PROG; bus_event_t e = {BUS_IF, a, v, cpu->fc, 2, bus_cycle}; on_bus(_CPUCA &e); bus_cycle += 4; }
	return v;
}
#endif

/* Instruction fetch */
/* Instruction stream fetch. When PREFETCH is enabled, F()/F16()/F32() use the... */
#ifndef PREFETCH
	#define F() (PC += 2, (u16)RAM_RD16((PC-2) & ADDR_MASK))
	#define F16() (PC += 2, (u32)(i16)(u16)RAM_RD16((PC-2) & ADDR_MASK))
	#define F32() (PC += 4, RAM_RD32((PC-4) & ADDR_MASK))
#else
	INLINE u16 pf_fetch(_CPUP) {
		u16 v = cpu->irc;
		PC += 2;
		cpu->irc = rd16(_CPUCA PC);
		return v;
	}
	INLINE u32 pf_fetch32(_CPUP) {
		u16 hi = pf_fetch(_CPUPA);
		u16 lo = pf_fetch(_CPUPA);
		return ((u32)hi << 16) | lo;
	}
	INLINE void pf_prime(_CPUP) {
		cpu->ird = rd16(_CPUCA PC);
		cpu->irc = rd16(_CPUCA (PC + 2));
	}
	#define F() pf_fetch(_CPUPA)
	#define F16() (u32)(i16)pf_fetch(_CPUPA)
	#define F32() pf_fetch32(_CPUPA)
#endif

#ifdef FAST_MEM
#define A7COM(r) ((void)0)
#else
#define A7COM(r) do { if ((r) == 7) a7_committed = A[7]; } while(0)
#endif

#ifdef FAST_MEM

/* Stack & A7 commit */
#define PUSH16(v) do { A[7] -= 2; wr16(_CPUCA A[7], (v)); } while(0)
#define PUSH32(v) do { A[7] -= 4; wr32(_CPUCA A[7], (v)); } while(0)
#define POP16() ({ u16 _v = rd16(_CPUCA A[7]); A[7] += 2; _v; })
#define POP32() ({ u32 _v = rd32(_CPUCA A[7]); A[7] += 4; _v; })
#else
#define PUSH16(v) do { A[7] -= 2; wr16(_CPUCA A[7], (v)); a7_committed = A[7]; } while(0)
#define PUSH32(v) do { A[7] -= 4; wr32(_CPUCA A[7], (v)); a7_committed = A[7]; } while(0)
#define POP16() ({ u16 _v = rd16(_CPUCA A[7]); A[7] += 2; a7_committed = A[7]; _v; })
#define POP32() ({ u32 _v = rd32(_CPUCA A[7]); A[7] += 4; a7_committed = A[7]; _v; })
#endif

#ifdef FAST_MEM
#define SET_DFPC(x) ((void)0)
#define SET_FIP(x) ((void)0)
#else
#define SET_DFPC(x) (data_fault_pc = (x))
#define SET_FIP(x) (fault_is_program = (x))
#endif

/* Condition codes */
INLINE int cc(_CPUC int cond) {
	u32 fn = flag_n, fz = flag_z;
	u32 fv = flag_v, fc = flag_c;
	switch (cond) {
	case 0: return 1;
	case 1: return 0;
	case 2: return !fc && fz;
	case 3: return fc || !fz;
	case 4: return !fc;
	case 5: return fc;
	case 6: return fz;
	case 7: return !fz;
	case 8: return !fv;
	case 9: return fv;
	case 10: return !fn;
	case 11: return fn;
	case 12: return !((fn ^ fv) & 0x80);
	case 13: return (fn ^ fv) & 0x80;
	case 14: return !((fn ^ fv) & 0x80) && fz;
	case 15: return ((fn ^ fv) & 0x80) || !fz;
	}
	return 0;
}

/* Flag macros */
#define SETNZ8(r) do { flag_n = (u8)(r) & 0x80; flag_z = (u8)(r); flag_v = 0; flag_c = 0; } while(0)
#define SETNZ16(r) do { flag_n = ((u16)(r) >> 15) << 7; flag_z = (u16)(r); flag_v = 0; flag_c = 0; } while(0)
#define SETNZ32(r) do { flag_n = ((u32)(r) >> 31) << 7; flag_z = (u32)(r); flag_v = 0; flag_c = 0; } while(0)
#define SETNZ_SZ(sz, r) do { if (sz==1) SETNZ8(r); else if (sz==2) SETNZ16(r); else SETNZ32(r); } while(0)
#define SETCX(carry) do { flag_c = (carry) ? 0x100 : 0; flag_x = (carry) ? 0x10 : 0; } while(0)
#define SETADD8(d,s,r) do { flag_n = (u8)(r) & 0x80; flag_z = (u8)(r); SETCX((r) & 0x100); flag_v = (~(d ^ s) & (d ^ r) & 0x80); } while(0)
#define SETADD16(d,s,r) do { flag_n = ((u16)(r) >> 15) << 7; flag_z = (u16)(r); SETCX((r) & 0x10000); flag_v = (~(d ^ s) & (d ^ r) & 0x8000) ? 0x80 : 0; } while(0)
#define SETADD32(d,s,r) do { flag_n = ((u32)(r) >> 31) << 7; flag_z = (u32)(r); SETCX((u32)(r) < (u32)(d)); flag_v = (~(d ^ s) & (d ^ (u32)(r)) & 0x80000000) ? 0x80 : 0; } while(0)
#define SETSUB8(d,s,r) do { flag_n = (u8)(r) & 0x80; flag_z = (u8)(r); SETCX(d < (u8)(s)); flag_v = ((d ^ s) & (d ^ r) & 0x80); } while(0)
#define SETSUB16(d,s,r) do { flag_n = ((u16)(r) >> 15) << 7; flag_z = (u16)(r); SETCX(d < (u16)(s)); flag_v = ((d ^ s) & (d ^ r) & 0x8000) ? 0x80 : 0; } while(0)
#define SETSUB32(d,s,r) do { flag_n = ((u32)(r) >> 31) << 7; flag_z = (u32)(r); SETCX(d < (u32)(s)); flag_v = ((d ^ s) & (d ^ (u32)(r)) & 0x80000000) ? 0x80 : 0; } while(0)
#define SETCMP8(d,s,r) do { flag_n = (u8)(r) & 0x80; flag_z = (u8)(r); flag_c = (d < (u8)(s)) ? 0x100 : 0; flag_v = ((d ^ s) & (d ^ r) & 0x80); } while(0)
#define SETCMP16(d,s,r) do { flag_n = ((u16)(r) >> 15) << 7; flag_z = (u16)(r); flag_c = (d < (u16)(s)) ? 0x100 : 0; flag_v = ((d ^ s) & (d ^ r) & 0x8000) ? 0x80 : 0; } while(0)
#define SETCMP32(d,s,r) do { flag_n = ((u32)(r) >> 31) << 7; flag_z = (u32)(r); flag_c = (d < (u32)(s)) ? 0x100 : 0; flag_v = ((d ^ s) & (d ^ (u32)(r)) & 0x80000000) ? 0x80 : 0; } while(0)
#define DO_ADD_SZ(sz,d,s,r) do { if (sz==1) { u16 _r=(u8)(d)+(u8)(s); SETADD8((u8)(d),(u8)(s),_r); r=(u8)_r; } else if (sz==2) { u32 _r=(u16)(d)+(u16)(s); SETADD16((u16)(d),(u16)(s),_r); r=(u16)_r; } else { uint64_t _r=(uint64_t)(d)+(s); SETADD32((u32)(d),(u32)(s),_r); r=(u32)_r; } } while(0)
#define DO_SUB_SZ(sz,d,s,r) do { if (sz==1) { u16 _r=(u8)(d)-(u8)(s); SETSUB8((u8)(d),(u8)(s),_r); r=(u8)_r; } else if (sz==2) { u32 _r=(u16)(d)-(u16)(s); SETSUB16((u16)(d),(u16)(s),_r); r=(u16)_r; } else { uint64_t _r=(uint64_t)(d)-(s); SETSUB32((u32)(d),(u32)(s),_r); r=(u32)_r; } } while(0)
#define DO_CMP_SZ(sz,d,s) do { if (sz==1) { u16 _r=(u8)(d)-(u8)(s); SETCMP8((u8)(d),(u8)(s),_r); } else if (sz==2) { u32 _r=(u16)(d)-(u16)(s); SETCMP16((u16)(d),(u16)(s),_r); } else { uint64_t _r=(uint64_t)(d)-(s); SETCMP32((u32)(d),(u32)(s),_r); } } while(0)

#define DO_NEGX_SZ(sz,val,r) do { \
u8 _xc = flag_x ? 1 : 0; u32 _sz = flag_z; \
flag_n = 0; flag_z = 0; flag_v = 0; flag_c = 0; flag_x = 0; \
if (sz == 1) { u16 _rr = 0 - (u16)(u8)(val) - _xc; \
r = (u8)_rr; flag_n = r & 0x80; \
if (r != 0) { flag_z = r; } if ((u8)(val) || _xc) SETCX(1); \
if ((0 ^ (u8)(val)) & (0 ^ r) & 0x80) flag_v = 0x80; \
} else if (sz == 2) { u32 _rr = 0 - (u32)(u16)(val) - _xc; \
r = (u16)_rr; flag_n = (r >> 15) << 7; \
if (r != 0) { flag_z = r; } if ((u16)(val) || _xc) SETCX(1); \
if ((0 ^ (u16)(val)) & (0 ^ r) & 0x8000) flag_v = 0x80; \
} else { uint64_t _rr = 0 - (uint64_t)(val) - _xc; \
r = (u32)_rr; flag_n = (r >> 31) << 7; \
if (r != 0) { flag_z = r; } if ((val) || _xc) SETCX(1); \
if ((0 ^ (val)) & (0 ^ r) & 0x80000000) flag_v = 0x80; \
} if (r == 0) flag_z = _sz; \
} while(0)

/* EA calculation & access */
static inline u32 ea_calc(_CPUC u8 mode, u8 reg, int sz) {
	u32 ia = PC - 2;
	SET_FIP(0);
	switch (mode) {
	case 2: SET_DFPC(ia + 2); return A[reg];
	case 3: SET_DFPC(ia + 2); {
		u32 a = A[reg];
		int inc = (sz == 1 && reg == 7) ? 2 : sz;
		if (sz == 4) {
			ea_pending_dec_reg = reg;
			ea_pending_dec_val = inc;
		}
		else {
			A[reg] += inc;
		}
		return a;
	}
	case 4: {
		SET_DFPC(ia + 2);
		int dec = (sz == 1 && reg == 7) ? 2 : sz;
		A[reg] -= dec;
		return A[reg];
	}
	case 5: SET_DFPC(ia + 2); { i16 d = (i16)F(); return A[reg] + d; }
	case 6: {
		SET_DFPC(ia + 2);
		u16 ext = F();
		i8 disp = (i8)(ext & 0xFF);
		u32 base = A[reg];
		u32 xn = (ext & 0x8000) ? A[(ext >> 12) & 7] : D[(ext >> 12) & 7];
		if (!(ext & 0x800)) xn = (u32)(i16)(xn & 0xFFFF);
		return base + disp + xn;
	}
	case 7:
		switch (reg) {
		case 0: { u32 v = (u32)(i16)F(); SET_DFPC(ia + 2); return v; }
		case 1: { u32 v = F32(); SET_DFPC(ia + 4); return v; }
		case 2: SET_FIP(1); { u32 base = PC; i16 d = (i16)F(); SET_DFPC(ia + 2); return base + d; }
		case 3: SET_FIP(1); {
			SET_DFPC(ia + 2);
			u16 ext = F();
			u32 base = PC - 2;
			i8 disp = (i8)(ext & 0xFF);
			u32 xn = (ext & 0x8000) ? A[(ext >> 12) & 7] : D[(ext >> 12) & 7];
			if (!(ext & 0x800)) xn = (u32)(i16)(xn & 0xFFFF);
			return base + disp + xn;
		}
		default: SET_DFPC(PC); return 0;
		}
	default: SET_DFPC(PC); return 0;
	}
}

INLINE int ea_ext_count(u8 mode, u8 reg, int sz) {
	switch (mode) {
	case 0: case 1: case 2: case 3: case 4: return 0;
	case 5: case 6: return 1;
	case 7:
		switch (reg) {
		case 0: case 2: case 3: return 1;
		case 1: return 2;
		case 4: return (sz == 4) ? 2 : 1;
		default: return 0;
		}
	default: return 0;
	}
}

#define DN_WRITE(reg, sz, val) do { \
if (sz == 1) D[reg] = (D[reg] & 0xFFFFFF00) | ((u8)(val)); \
else if (sz == 2) D[reg] = (D[reg] & 0xFFFF0000) | ((u16)(val)); \
else D[reg] = (val); \
} while(0)
#define DECODE_SZ(sz_field) ((sz_field) == 0 ? 1 : (sz_field) == 1 ? 2 : 4)
#define WR_SZ(addr, sz, val) do { if (sz == 1) wr8(_CPUCA addr, val); else if (sz == 2) wr16(_CPUCA addr, val); else wr32(_CPUCA addr, val); } while(0)
#define RD_SZ(addr, sz) (sz == 1 ? rd8(_CPUCA addr) : sz == 2 ? rd16(_CPUCA addr) : rd32(_CPUCA addr))
#ifdef FAST_MEM
#define EA_ADDR(mode, reg, sz) ea_calc(_CPUCA mode, reg, sz)
#define EA_COMMIT_POST() do { if (ea_pending_dec_reg >= 0) { A[ea_pending_dec_reg] += ea_pending_dec_val; ea_pending_dec_reg = -1; } } while(0)
#else
#define EA_ADDR(mode, reg, sz) ea_calc(_CPUCA mode, reg, sz)
#define EA_COMMIT_POST() do { if (ea_pending_dec_reg >= 0) { A[ea_pending_dec_reg] += ea_pending_dec_val; if (ea_pending_dec_reg == 7) a7_committed = A[7]; ea_pending_dec_reg = -1; } } while(0)
#endif

#define EA_DN_READ(val, addr, mode, reg, sz) do { \
if (mode == 0) { val = D[reg]; } \
else { addr = EA_ADDR(mode, reg, sz); val = RD_SZ(addr, sz); EA_COMMIT_POST(); } \
} while(0)
#define EA_DN_WRITE(reg, sz, result, addr, mode) do { \
if (mode == 0) { DN_WRITE(reg, sz, result); } \
else { ea_write_addr(_CPUCA addr, sz, result); } \
} while(0)

/* ALU helpers */
/* ALU_LOGICAL: OR/AND/EOR, then SETNZ */
#define ALU_LOGICAL(op, sz, rn, mode, reg, result) do { \
	u32 val, _addr = 0; \
	if (op & OP_DIR) { \
		EA_DN_READ(val, _addr, mode, reg, sz); \
		result = D[rn] | val; \
		EA_DN_WRITE(reg, sz, result, _addr, mode); \
	} else { \
		val = ea_read(_CPUCA mode, reg, sz); \
		result = D[rn] | val; \
		DN_WRITE(rn, sz, result); \
	} \
	SETNZ_SZ(sz, result); \
	CYC(alu_cyc(sz, (op & OP_DIR) != 0, mode)); \
} while(0)

/* ALU_LOGICAL_AND: AND, byte-size early-out */
#define ALU_LOGICAL_AND(op, sz, rn, mode, reg, result) do { \
	if (sz == 0) { CYC(4); break; } \
	u32 val, _addr = 0; \
	if (op & OP_DIR) { \
		EA_DN_READ(val, _addr, mode, reg, sz); \
		result = D[rn] & val; \
		EA_DN_WRITE(reg, sz, result, _addr, mode); \
	} else { \
		val = ea_read(_CPUCA mode, reg, sz); \
		result = D[rn] & val; \
		DN_WRITE(rn, sz, result); \
	} \
	SETNZ_SZ(sz, result); \
	CYC(alu_cyc(sz, (op & OP_DIR) != 0, mode)); \
} while(0)

/* ALU_ARITH: SUB/ADD with flag macros */
#define ALU_ARITH(flag_macro, sz, op, rn, mode, reg, result) do { \
	u32 val, _addr = 0; \
	if (op & OP_DIR) { \
		EA_DN_READ(val, _addr, mode, reg, sz); \
		flag_macro(sz, val, D[rn], result); \
		EA_DN_WRITE(reg, sz, result, _addr, mode); \
	} else { \
		val = ea_read(_CPUCA mode, reg, sz); \
		u32 _dst = D[rn]; \
		flag_macro(sz, _dst, val, result); \
		DN_WRITE(rn, sz, result); \
	} \
	CYC(alu_cyc(sz, (op & OP_DIR) != 0, mode)); \
} while(0)

/* ADDX/SUBX & BCD */
static inline void do_addx_subx(_CPUC int is_add, int real_sz, u8 rn, u8 mode, u8 xr) {
	u32 src, dst, result, write_addr = 0;
	if (mode == 0) {
		src = D[xr] & (real_sz == 1 ? 0xFF : real_sz == 2 ? 0xFFFF : ~0u);
		dst = D[rn] & (real_sz == 1 ? 0xFF : real_sz == 2 ? 0xFFFF : ~0u);
	}
	else {
		SET_DFPC(instr_addr); SET_FIP(0);
		int src_dec = (real_sz == 1 && xr == 7) ? 2 : real_sz;
		int dst_dec = (real_sz == 1 && rn == 7) ? 2 : real_sz;
		if (real_sz == 4) {
			A[xr] -= 2; if (xr == 7) A7COM(7);
			u16 src_lo = rd16(_CPUCA A[xr]);
			A[xr] -= 2; if (xr == 7) A7COM(7);
			u16 src_hi = rd16(_CPUCA A[xr]);
			src = ((u32)src_hi << 16) | src_lo;
			A[rn] -= 2; if (rn == 7) A7COM(7);
			u16 dst_lo = rd16(_CPUCA A[rn]);
			A[rn] -= 2; if (rn == 7) A7COM(7);
			u16 dst_hi = rd16(_CPUCA A[rn]);
			dst = ((u32)dst_hi << 16) | dst_lo;
			write_addr = A[rn];
		}
		else {
			A[xr] -= src_dec; if (xr == 7) A7COM(7);
			src = (real_sz == 1) ? rd8(_CPUCA A[xr]) : rd16(_CPUCA A[xr]);
			A[rn] -= dst_dec; if (rn == 7) A7COM(7);
			dst = (real_sz == 1) ? rd8(_CPUCA A[rn]) : rd16(_CPUCA A[rn]);
			write_addr = A[rn];
		}
	}
	u8 xc = flag_x ? 1 : 0;
	flag_n = 0; flag_v = 0; flag_c = 0; flag_x = 0;
	if (is_add) {
		if (real_sz == 1) {
			u16 r = (u16)(u8)dst + (u16)(u8)src + xc;
			result = (u8)r; flag_n = result & 0x80;
			if (result) flag_z = result;
			if (r > 0xFF) SETCX(1);
			if ((~((u8)dst ^ (u8)src)) & ((u8)dst ^ result) & 0x80) flag_v = 0x80;
		}
		else if (real_sz == 2) {
			u32 r = (u32)(u16)dst + (u32)(u16)src + xc;
			result = (u16)r; flag_n = ((u16)(result) >> 15) << 7;
			if (result) flag_z = result;
			if (r > 0xFFFF) SETCX(1);
			if ((~((u16)dst ^ (u16)src)) & ((u16)dst ^ (u16)result) & 0x8000) flag_v = 0x80;
		}
		else {
			uint64_t r = (uint64_t)dst + src + xc;
			result = (u32)r; flag_n = ((u32)(result) >> 31) << 7;
			if (result) flag_z = result;
			if (r > ~0u) SETCX(1);
			if ((~(dst ^ src)) & (dst ^ result) & 0x80000000) flag_v = 0x80;
		}
	}
	else {
		if (real_sz == 1) {
			u16 r = (u16)(u8)dst - (u16)(u8)src - xc;
			result = (u8)r; flag_n = result & 0x80;
			if (result) flag_z = result;
			if ((u16)(u8)dst < (u16)(u8)src + xc) SETCX(1);
			if (((u8)dst ^ (u8)src) & ((u8)dst ^ result) & 0x80) flag_v = 0x80;
		}
		else if (real_sz == 2) {
			u32 r = (u32)(u16)dst - (u32)(u16)src - xc;
			result = (u16)r; flag_n = ((u16)(result) >> 15) << 7;
			if (result) flag_z = result;
			if ((u32)(u16)dst < (u32)(u16)src + xc) SETCX(1);
			if (((u16)dst ^ (u16)src) & ((u16)dst ^ (u16)result) & 0x8000) flag_v = 0x80;
		}
		else {
			uint64_t r = (uint64_t)dst - src - xc;
			result = (u32)r; flag_n = ((u32)(result) >> 31) << 7;
			if (result) flag_z = result;
			if ((uint64_t)dst < (uint64_t)src + xc) SETCX(1);
			if ((dst ^ src) & (dst ^ result) & 0x80000000) flag_v = 0x80;
		}
	}
	if (mode == 0) DN_WRITE(rn, real_sz, result);
	else { WR_SZ(write_addr, real_sz, result); }
}

static inline void do_bcd(_CPUC int is_add, u8 rn, u8 mode, u8 xr) {
	u8 src, dst;
	if (mode == 0) { src = (u8)(D[xr] & 0xFF); dst = (u8)(D[rn] & 0xFF); }
	else {
		SET_DFPC(instr_addr); SET_FIP(0);
		A[xr] -= (xr == 7) ? 2 : 1; src = rd8(_CPUCA A[xr]); if (xr == 7) A7COM(7);
		A[rn] -= (rn == 7) ? 2 : 1; dst = rd8(_CPUCA A[rn]); if (rn == 7) A7COM(7);
	}
	u8 xc = flag_x ? 1 : 0;
	u8 result;
	if (is_add) {
		u16 tmp = (u16)src + (u16)dst + xc;
		if (((src & 0xF) + (dst & 0xF) + xc) > 9) tmp += 6;
		u8 carry = 0;
		if ((tmp & 0xF0) > 0x90 || tmp > 0xFF) { tmp += 0x60; carry = 1; }
		result = (u8)tmp;
		if (result) flag_z = result;
		flag_n = result & 0x80;
		{
			u8 bin_result = (u8)(src + dst + xc);
			flag_v = (!(bin_result & 0x80) && (result & 0x80)) ? 0x80 : 0;
		}
		SETCX(carry);
	}
	else {
		u8 bin_result = (u8)(dst - src - xc);
		int low_borrow = ((src & 0xF) + xc > (dst & 0xF));
		int borrow = (src + xc > dst);
		result = bin_result;
		if (low_borrow) result -= 6;
		if (borrow) result -= 0x60;
		if (result) flag_z = result;
		flag_n = result & 0x80;
		flag_v = (bin_result & 0x80 && !(result & 0x80)) ? 0x80 : 0;
		int bcd_borrow = borrow || (low_borrow && (bin_result >> 4) == 0 && (bin_result & 0x0F) < 6);
		SETCX(bcd_borrow);
	}
	if (mode == 0) D[rn] = (D[rn] & 0xFFFFFF00) | result;
	else { wr8(_CPUCA A[rn], result); }
}

INLINE void ea_write_addr(_CPUC u32 addr, int sz, u32 val);

/* EA read/write with save */
static inline u32 ea_read_save(_CPUC u8 mode, u8 reg, int sz, u32* saved_addr) {
	CYC(mode == 7 ? ea_cyc7(reg, sz) : ea_cyc(mode, sz));
	if (mode == 0) { *saved_addr = 0; return sz == 1 ? (D[reg] & 0xFF) : sz == 2 ? (D[reg] & 0xFFFF) : D[reg]; }
	if (mode == 1) { *saved_addr = 0; return sz == 2 ? (A[reg] & 0xFFFF) : A[reg]; }
	if (mode == 7 && reg == 4) { *saved_addr = 0; SET_DFPC(instr_addr + (sz == 4 ? 4 : 2)); if (sz == 1) return F() & 0xFF; if (sz == 2) return F(); return F32(); }
	u32 addr = ea_calc(_CPUCA mode, reg, sz);
	*saved_addr = addr;
	if (sz == 1) { u8 v = rd8(_CPUCA addr); EA_COMMIT_POST(); if (reg == 7 && (mode == 4 || (mode == 3 && sz != 4))) A7COM(7); return v; }
	if (sz == 2) { u16 v = rd16(_CPUCA addr); EA_COMMIT_POST(); if (reg == 7 && (mode == 4 || (mode == 3 && sz != 4))) A7COM(7); return v; }
	u32 val = rd32(_CPUCA addr);
	EA_COMMIT_POST();
	if (reg == 7 && (mode == 4 || (mode == 3 && sz != 4))) A7COM(7);
	return val;
}

INLINE u32 ea_read(_CPUC u8 mode, u8 reg, int sz) {
	u32 dummy;
	return ea_read_save(_CPUCA mode, reg, sz, &dummy);
}

static inline void ea_write_saved(_CPUC u8 mode, u8 reg, int sz, u32 val, u32 saved_addr) {
	if (mode == 0) { DN_WRITE(reg, sz, val); return; }
	if (mode == 1) { A[reg] = (sz == 2) ? (u32)(i16)(val & 0xFFFF) : val; return; }
	if (mode == 7 && reg == 4) return;
	ea_write_addr(_CPUCA saved_addr, sz, val);
}

INLINE void ea_write(_CPUC u8 mode, u8 reg, int sz, u32 val) {
	CYC(mode == 7 ? ea_cyc7(reg, sz) : ea_cyc(mode, sz));
	if (mode == 0) { DN_WRITE(reg, sz, val); return; }
	if (mode == 1) { A[reg] = (sz == 2) ? (u32)(i16)(val & 0xFFFF) : val; return; }
	if (mode == 7 && reg == 4) return;
	u32 addr = ea_calc(_CPUCA mode, reg, sz);
	WR_SZ(addr, sz, val);
	EA_COMMIT_POST();
	if (reg == 7 && (mode == 4 || (mode == 3 && sz != 4))) A7COM(7);
}
INLINE void ea_write_addr(_CPUC u32 addr, int sz, u32 val) { WR_SZ(addr, sz, val); }

/* Shift/rotate */
static inline u32 shift_rot(_CPUC int op, u32 val, int cnt, int sz) {
	u32 mask = sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : ~0u;
	int bits = sz == 1 ? 8 : sz == 2 ? 16 : 32;
	u32 result = val & mask;
	u32 msb_bit = 1u << (bits - 1);
	if (cnt == 0) {
		u32 saved_x = flag_x;
		SETNZ_SZ(sz, result);
		flag_x = saved_x; flag_v = 0;
		if (op == 6 || op == 7) flag_c = saved_x ? 0x100 : 0;
		return result;
	}
	if (op == 1 && cnt > bits) {
		int sign_bit = (result & msb_bit) ? 1 : 0;
		result = sign_bit ? mask : 0;
		SETNZ_SZ(sz, result);
		flag_c = sign_bit ? 0x100 : 0; flag_x = sign_bit ? 0x10 : 0; flag_v = 0;
		return result;
	}
	if (op == 2) {
		u32 carry;
		if (cnt >= bits) {
			carry = (cnt == bits) ? (result & 1) : 0;
			result = 0;
		}
		else {
			carry = (result >> (bits - cnt)) & 1;
			result = (result << cnt) & mask;
		}
		SETNZ_SZ(sz, result);
		flag_c = carry ? 0x100 : 0; flag_x = carry ? 0x10 : 0; flag_v = 0;
		return result;
	}
	if (op == 3) {
		u32 carry;
		if (cnt >= bits) {
			carry = (cnt == bits) ? ((result >> (bits - 1)) & 1) : 0;
			result = 0;
		}
		else {
			carry = (result >> (cnt - 1)) & 1;
			result = result >> cnt;
		}
		SETNZ_SZ(sz, result);
		flag_c = carry ? 0x100 : 0; flag_x = carry ? 0x10 : 0; flag_v = 0;
		return result;
	}
	if (op == 4) {
		u32 orig = result;
		int sc = cnt % bits;
		if (sc) result = ((result << sc) | (result >> (bits - sc))) & mask;
		SETNZ_SZ(sz, result);

		int cbit = sc ? (bits - sc) : 0;
		flag_c = (orig >> cbit) & 1 ? 0x100 : 0; flag_v = 0;
		return result;
	}
	if (op == 5) {
		u32 orig = result;
		int sc = cnt % bits;
		if (sc) result = ((result >> sc) | (result << (bits - sc))) & mask;
		SETNZ_SZ(sz, result);

		int cbit = sc ? (sc - 1) : (bits - 1);
		flag_c = (orig >> cbit) & 1 ? 0x100 : 0; flag_v = 0;
		return result;
	}

	int asl_v = 0;
	for (int i = 0; i < cnt; i++) {
		u32 old_x = flag_x ? 1 : 0;
		flag_c = 0;
		if (op != 4 && op != 5) flag_x = 0;
		switch (op) {
		case 0:
			if (result & msb_bit) { flag_c = 0x100; flag_x = 0x10; }
			{ u32 old = result; result = (result << 1) & mask; if ((old ^ result) & msb_bit) asl_v = 1; } break;
		case 1:
			if (result & 1) { flag_c = 0x100; flag_x = 0x10; }
			{ u32 sign = result & msb_bit; result >>= 1; if (sign) result |= (mask & ~(mask >> 1)); } break;
		case 6:
		{ u32 msb = (result >> (bits - 1)) & 1; if (msb) { flag_c = 0x100; flag_x = 0x10; } result = ((result << 1) | old_x) & mask; } break;
		case 7:
		{ u32 lsb = result & 1; if (lsb) { flag_c = 0x100; flag_x = 0x10; } result = ((result >> 1) | (old_x << (bits - 1))) & mask; } break;
		}
	}
	u32 saved_cx = flag_c | flag_x;
	SETNZ_SZ(sz, result);
	flag_c = saved_cx & 0x100;
	flag_x = saved_cx & 0x10;
	if (op == 0) flag_v = asl_v ? 0x80 : 0; else flag_v = 0;
	return result;
}

/* Exception handling */
INLINE void enter_super(_CPUP) {
	int was_user = !(SR & SR_S);
	SR |= SR_S;
	SR &= ~SR_T;
	if (was_user) { u32 tsp = USP; USP = A[7]; A[7] = tsp; }
}

static void exception_common(_CPUC int vector, u32 push_pc) {
	check_align = 0;
	u16 old_sr = get_sr(_CPUPA);
	enter_super(_CPUPA);
	PUSH32(push_pc);
	PUSH16(old_sr);
	PC = rd32(_CPUCA(u32)vector * 4);
#ifdef PREFETCH
	PF_INVALIDATE();
#endif
	CYC(34);
}

#define exception_fault(v) exception_common(_CPUCA (v), instr_addr)
#define exception_trap(v) exception_common(_CPUCA (v), PC)

static void addr_error_exception(_CPUC u32 fault_addr_val,
	u32 fault_pc, u16 opcode, int is_read, int is_insn) {
	if (cpu->fault_depth >= 1) {
		cpu->stopped = 2; /* double-fault halt */
		PC = instr_addr;
#ifdef PREFETCH
		PF_INVALIDATE();
#endif
		return;
	}
	cpu->fault_depth = 1;
	addr_error_fired = 1;
	cpu->ae_fired = 1;
	u16 old_sr = get_sr(_CPUPA);
	enter_super(_CPUPA);
	int is_supervisor = old_sr & SR_S;
	int fc = (is_supervisor ? 4 : 0) | (is_insn ? 2 : 1);

	u16 ssw = (opcode & SSW_IR_MASK) | (is_read ? SSW_RW_BIT : 0) | (is_insn ? 0x08 : 0) | fc;
	PUSH32(fault_pc);
	PUSH16(old_sr);
	PUSH16(opcode);
	PUSH32(fault_addr_val);
	PUSH16(ssw);
	PC = rd32(_CPUCA VEC_ADDRERR * 4);
#ifdef PREFETCH
	PF_INVALIDATE();
#endif
	CYC(50);
	cpu->fault_depth = 0;
}

/* Public API */
void cpu68000_init(_CPUC u8* ram_ptr, cpu68000_read_cb rd_cb, cpu68000_write_cb wr_cb, cpu68000_intack_cb ia_cb) {
#ifdef SINGLE_INST
	memset(&g_cpu, 0, sizeof(g_cpu));
#else
	memset(cpu, 0, sizeof(*cpu));
	cpu->fault = (CPU68000Fault*)calloc(1, sizeof(CPU68000Fault));
#endif
	ram = ram_ptr;
#ifdef MEM_IO
	mem_read = rd_cb ? rd_cb : default_rd;
	mem_write = wr_cb ? wr_cb : default_wr;
#else
	mem_read = rd_cb;
	mem_write = wr_cb;
#endif
	int_ack = ia_cb ? ia_cb : intack_default;
	check_align = 0;
	ea_pending_dec_reg = -1;
#ifndef FAST_MEM
	dbcc_pending_reg = -1;
#endif
#ifdef SINGLE_INST
	cpu68000_get();
#endif
}

void cpu68000_reset(_CPUP) {
	int i;
	for (i = 0; i < 8; i++) { D[i] = 0; A[i] = 0; }
	USP = 0;
	SR = SR_S | SR_IM2 | SR_IM1 | SR_IM0;
	set_sr_flags(_CPUCA SR_S | SR_IM2 | SR_IM1 | SR_IM0);
	A[7] = rd32(_CPUCA VEC_RESET_SSP * 4);
	PC = rd32(_CPUCA VEC_RESET_PC * 4);
	check_align = 0;
	ea_pending_dec_reg = -1;
	cpu->stopped = 0;
	cpu->trace_pending = 0;
	cpu->fault_depth = 0;
	cpu->ipl = 0;
	cpu->irq_vector = -1;
#ifdef PREFETCH
	cpu->irc = 0; cpu->ird = 0;
#endif
#ifdef BUS_HOOKS
	cpu->fc = FC_SUPER_PROG;
#endif
#ifndef FAST_MEM
	dbcc_pending_reg = -1;
	prev_sp = 0;
	prev_opcode = 0;
#endif
#ifdef SINGLE_INST
	cpu68000_get();
#endif
}

void cpu68000_irq(_CPUC int level, int vector) {
	if (level <= ((SR >> 8) & 7)) return;
	cpu->stopped = 0;
	check_align = 0;
	cpu->trace_pending = 0;
	u16 old_sr = get_sr(_CPUPA);
	enter_super(_CPUPA);
	PUSH32(PC);
	PUSH16(old_sr);
	if (vector < 0) vector = int_ack(_CPUCA level);
	PC = rd32(_CPUCA(u32)vector * 4);
#ifdef PREFETCH
	PF_INVALIDATE();
#endif
	CYC(44);
}

u16 cpu68000_get_sr(_CCPUP) { return get_sr(_CPUPA); }
void cpu68000_set_sr(_CPUC u16 sr) { set_sr_flags(_CPUCA sr); }

void cpu68000_set_ipl(_CPUC u8 level, int vector) {
	cpu->ipl = level;
	cpu->irq_vector = (i8)vector;
}

size_t cpu68000_state_size(_CCPUP) { return 8*4 + 8*4 + 4 + 4 + 2 + 5*4 + 1; }
#ifndef SINGLE_INST
void cpu68000_save(_CPUC u8* buf) {
	memcpy(buf, D, 32); buf += 32;
	memcpy(buf, A, 32); buf += 32;
	memcpy(buf, &USP, 4); buf += 4;
	memcpy(buf, &PC, 4); buf += 4;
	memcpy(buf, &SR, 2); buf += 2;
	memcpy(buf, &flag_n, 20);
}
void cpu68000_load(_CPUC const u8* buf) {
	memcpy(D, buf, 32); buf += 32;
	memcpy(A, buf, 32); buf += 32;
	memcpy(&USP, buf, 4); buf += 4;
	memcpy(&PC, buf, 4); buf += 4;
	memcpy(&SR, buf, 2); buf += 2;
	memcpy(&flag_n, buf, 20);
}
#endif

#ifdef DEBUG
void cpu68000_dump(_CCPUP) {
	printf("D0=%08X D1=%08X D2=%08X D3=%08X\n", D[0], D[1], D[2], D[3]);
	printf("D4=%08X D5=%08X D6=%08X D7=%08X\n", D[4], D[5], D[6], D[7]);
	printf("A0=%08X A1=%08X A2=%08X A3=%08X\n", A[0], A[1], A[2], A[3]);
	printf("A4=%08X A5=%08X A6=%08X A7=%08X\n", A[4], A[5], A[6], A[7]);
	printf("PC=%08X SR=%04X USP=%08X\n", PC, get_sr(_CPUPA), USP);
}

#endif

CYCLES cpu68000_step(_CPUP)
{
	addr_error_fired = 0;
	cpu->ae_fired = 0;
	STEPSTART
	if (UNLIKELY(cpu->stopped)) {
#ifdef COUNT_CYCLES
		cpu->cycles = (cpu->stopped == 1) ? 2 : 0;
#endif
		return STEPRET;
	}

	if (UNLIKELY(cpu->trace_pending)) {
		cpu->trace_pending = 0;
		u16 t_sr = get_sr(_CPUPA);
		enter_super(_CPUPA);
		PUSH32(PC);
		PUSH16(t_sr);
		PC = rd32(_CPUCA 9 * 4);
#ifdef PREFETCH
		PF_INVALIDATE();
#endif
		CYC(34);
		cpu->stopped = 0;
		return STEPRET;
	}
	cpu->trace_pending = (SR & SR_T) ? 1 : 0;
#ifdef FAST_MEM

		u32 op_addr = PC;
	instr_addr = op_addr;
	SET_DFPC(op_addr + 2);

	u32 sp_at_step_start = A[7];
	ea_pending_dec_reg = -1;
#ifdef PREFETCH
	pf_prime(_CPUPA);
	u16 op = cpu->ird;
	data_fault_ir = cpu->irc;
	PC += 2;
#else
#ifdef BUS_HOOKS
	u16 op = on_bus ? (PC += 2, bhv_ifetch(_CPUCA PC - 2)) : F();
#else
	u16 op = F();
#endif
#endif
#else
		u32 op_addr = PC;
	instr_addr = op_addr;
	SET_DFPC(op_addr + 2);

	u32 sp_at_step_start = A[7];
	if (UNLIKELY(op_addr & 1)) {
		dbcc_pending_reg = -1;
		int is_jsr = ((prev_opcode & 0xFFC0) == 0x4E80);
		if (is_jsr) {
			A[7] = prev_sp;
		}
		addr_error_exception(_CPUCA op_addr,
			op_addr - 4, prev_opcode, 1, 1);
		return STEPRET;
	}
	a7_committed = sp_at_step_start;
	check_align = 1;
	ea_pending_dec_reg = -1;
	dbcc_pending_reg = -1;
	SET_FIP(0);

#ifdef PREFETCH
	pf_prime(_CPUPA);
	u16 op = cpu->ird;
	data_fault_ir = cpu->irc;
	PC += 2;
#else
#ifdef BUS_HOOKS
	u16 op = on_bus ? (PC += 2, bhv_ifetch(_CPUCA PC - 2)) : F();
#else
	u16 op = F();
#endif
	data_fault_ir = op;
#endif

	ea_pending_dec_reg = -1;
	dbcc_pending_reg = -1;
#endif

	u8 mode = (op >> 3) & 7;
	u8 reg = op & 7;
	switch (op >> 12) {
	case 0x0: {
		u8 sz_field = (op >> 6) & 3;
		u8 rn = (op >> 9) & 7;
		if (mode == 1) {
			int dir = (op >> 7) & 1;
			int mpsz = (op & 0x40) ? 4 : 2;
			u16 disp = F();
			u32 addr = A[reg] + (i16)disp;
			if (dir == 0) {
				if (mpsz == 2) {
					D[rn] = (D[rn] & 0xFFFF0000) | ((u32)rd8(_CPUCA addr) << 8) | rd8(_CPUCA addr + 2);
				}
				else {
					D[rn] = ((u32)rd8(_CPUCA addr) << 24)
						| ((u32)rd8(_CPUCA addr + 2) << 16)
						| ((u32)rd8(_CPUCA addr + 4) << 8)
						| rd8(_CPUCA addr + 6);
				}
			}
			else {
				if (mpsz == 2) {
					wr8(_CPUCA addr, (D[rn] >> 8) & 0xFF);
					wr8(_CPUCA addr + 2, D[rn] & 0xFF);
				}
				else {
					wr8(_CPUCA addr, (D[rn] >> 24) & 0xFF);
					wr8(_CPUCA addr + 2, (D[rn] >> 16) & 0xFF);
					wr8(_CPUCA addr + 4, (D[rn] >> 8) & 0xFF);
					wr8(_CPUCA addr + 6, D[rn] & 0xFF);
				}
			}
			CYC(16); break;
		}
		if (!(op & OP_DIR) && mode == 7 && reg == 4 && rn == 0) {
			if (sz_field == 0) {
				u16 imm = F(); u16 sr = get_sr(_CPUPA);
				sr |= (imm & CCR_MASK); set_sr_flags(_CPUCA sr);
				SR = (SR & ~CCR_MASK) | (sr & CCR_MASK); CYC(20); break;
			}
			if (sz_field == 1 || sz_field == 2) {
				if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				u16 imm = F(); write_sr(_CPUCA get_sr(_CPUPA) | imm);
				CYC(20); break;
			}
		}
		if (mode != 1 && ((!(op & OP_DIR) && rn == 4) || (op & OP_DIR))) {
			u8 bit = (op & OP_DIR) ? (D[rn] & (mode == 0 ? 31 : 7)) : ((u8)F() & (mode == 0 ? 31 : 7));
			int bsz = mode == 0 ? 4 : 1;
			u32 ea_addr;
			u32 val = ea_read_save(_CPUCA mode, reg, bsz, &ea_addr);
			switch (sz_field) {
			case 0: flag_z = val & (1u << bit); CYC(op & OP_DIR ? 4 : 8); break;
			case 1: flag_z = val & (1u << bit); val ^= (1u << bit); ea_write_saved(_CPUCA mode, reg, bsz, val, ea_addr); CYC(8); break;
			case 2: flag_z = val & (1u << bit); val &= ~(1u << bit); ea_write_saved(_CPUCA mode, reg, bsz, val, ea_addr); CYC(8); break;
			case 3: flag_z = val & (1u << bit); val |= (1u << bit); ea_write_saved(_CPUCA mode, reg, bsz, val, ea_addr); CYC(8); break;
			}
			break;
		}
		{
			int real_sz = DECODE_SZ(sz_field);
			if (real_sz == 0) { CYC(4); break; }
			u32 imm;
			if (real_sz == 1) imm = F() & 0xFF;
			else if (real_sz == 2) imm = F();
			else imm = F32();
			if (mode == 7 && reg == 4) {
				u8 sub = (op >> 9) & 7;
				if (sub == 0 || sub == 1 || sub == 5) {
					if (real_sz == 1) {
						u16 sr = get_sr(_CPUPA);
						u8 ccr_mask = (u8)(imm & CCR_MASK);
						switch (sub) {
						case 0: sr |= ccr_mask; break;
						case 1: sr = (sr & ~CCR_MASK) | (sr & ccr_mask); break;
						case 5: sr ^= ccr_mask; break;
						}
						set_sr_flags(_CPUCA sr);
						SR = (SR & ~CCR_MASK) | (sr & CCR_MASK);
						CYC(20); break;
					}
					if (real_sz == 2 && supervisor(_CPUPA)) {
						u16 cur_sr = get_sr(_CPUPA);
						switch (sub) {
						case 0: write_sr(_CPUCA cur_sr | imm); break;
						case 1: write_sr(_CPUCA cur_sr & imm); break;
						case 5: write_sr(_CPUCA cur_sr ^ imm); break;
						}
						CYC(20); break;
					}
					if (real_sz == 2 && !supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				}
				else {
					exception_fault(VEC_ILLEGAL); break;
				}
			}
			u32 ea_addr;
			u32 val = ea_read_save(_CPUCA mode, reg, real_sz, &ea_addr);
			u32 result;
			switch (rn) {
			case 0: result = val | imm; ea_write_saved(_CPUCA mode, reg, real_sz, result, ea_addr);
				SETNZ_SZ(real_sz, result);
				CYC((real_sz == 4 ? 16 : 8) + (mode >= 2 ? ea_cyc(mode, real_sz) : 0));
				break;
			case 1: result = val & imm; ea_write_saved(_CPUCA mode, reg, real_sz, result, ea_addr);
				SETNZ_SZ(real_sz, result);
				CYC((real_sz == 4 ? 16 : 8) + (mode >= 2 ? ea_cyc(mode, real_sz) : 0));
				break;
			case 2: { u32 res; DO_SUB_SZ(real_sz, val, imm, res); ea_write_saved(_CPUCA mode, reg, real_sz, res, ea_addr); }
				CYC((real_sz == 4 ? 16 : 8) + (mode >= 2 ? ea_cyc(mode, real_sz) : 0));
				break;
			case 3: { u32 res; DO_ADD_SZ(real_sz, val, imm, res); ea_write_saved(_CPUCA mode, reg, real_sz, res, ea_addr); }
				CYC((real_sz == 4 ? 16 : 8) + (mode >= 2 ? ea_cyc(mode, real_sz) : 0));
				break;
			case 5: result = val ^ imm; ea_write_saved(_CPUCA mode, reg, real_sz, result, ea_addr);
				SETNZ_SZ(real_sz, result);
				CYC((real_sz == 4 ? 16 : 8) + (mode >= 2 ? ea_cyc(mode, real_sz) : 0));
				break;
			case 6: DO_CMP_SZ(real_sz, val, imm);
				CYC(8); break;
			default: CYC(4); break;
			}
		}
		break;
	}
	case 0x01: case 0x02: case 0x03: {
		int sz = (op >> 12) == 1 ? 1 : (op >> 12) == 2 ? 4 : 2;
		u8 sm = (op >> 3) & 7, sr = op & 7;
		u8 dm = (op >> 6) & 7, dr = (op >> 9) & 7;
		SET_DFPC(instr_addr);
		u32 val = ea_read(_CPUCA sm, sr, sz);

#ifndef FAST_MEM
		int src_ext = ea_ext_count(sm, sr, sz);
		int dest_ext = ea_ext_count(dm, dr, sz);
		int source_no_bus = (sm == 0 || sm == 1 || (sm == 7 && sr == 4));
		int dest_offset = source_no_bus
			? (dest_ext > 0 ? dest_ext : (dm >= 4 ? 1 : 0))
			: (dm >= 4 ? 1 : 0);
		u32 write_fault_pc = instr_addr + 2 * (src_ext + dest_offset);
#endif

		A7COM(7);
		if (dm != 1) { SETNZ_SZ(sz, val); }
		SET_DFPC(write_fault_pc);
		SET_FIP(0);
		if (dm == 1) {
			if (sz == 1) { exception_fault(VEC_ILLEGAL); break; }
			if (sz == 2) A[dr] = (u32)(i16)(val & 0xFFFF);
			else A[dr] = val;
		}
		else if (dm == 0) {
			DN_WRITE(dr, sz, val);
		}
		else if (dm == 3 || dm == 4) {
			int inc = (sz == 1 && dr == 7) ? 2 : sz;
			u32 addr = (dm == 4) ? A[dr] - inc : A[dr];
			if (dm == 4) {
				if (sz == 4) {
					A[dr] -= 2;
					if (dr == 7) A7COM(7);
					wr16(_CPUCA A[dr], val & 0xFFFF);
					A[dr] -= 2;
					if (dr == 7) A7COM(7);
					wr16(_CPUCA A[dr], (val >> 16) & 0xFFFF);
				}
				else {
					A[dr] = addr;
					if (dr == 7) A7COM(7);
					if (sz == 1) wr8(_CPUCA addr, val & 0xFF);
					else wr16(_CPUCA addr, val & 0xFFFF);
				}
			}
			else {
				if (sz == 1) wr8(_CPUCA addr, val & 0xFF);
				else if (sz == 2) wr16(_CPUCA addr, val & 0xFFFF);
				else {
					wr16(_CPUCA addr, (val >> 16) & 0xFFFF);
					wr16(_CPUCA addr + 2, val & 0xFFFF);
				}
				A[dr] = addr + inc;
				if (dr == 7) A7COM(7);
			}
			goto move_done;
		}
		else {
			u32 addr = ea_calc(_CPUCA dm, dr, sz);
			SET_DFPC(write_fault_pc);
			WR_SZ(addr, sz, val);
			EA_COMMIT_POST();
			if (dr == 7) A7COM(7);
		}
	move_done:
		CYC(4 + (dm == 7 ? ea_cyc7(dr, sz) : ea_cyc(dm, sz)));
		break;
	}
	case 0x04: {
		u8 rn = (op >> 9) & 7;
		u8 sz_field = (op >> 6) & 3;
		int real_sz = DECODE_SZ(sz_field);
		if ((op & 0xFFC0) == 0x4840 && mode == 0) {
			u32 v = D[op & 7];
			D[op & 7] = (v << 16) | (v >> 16);
			SETNZ32(D[op & 7]);
			CYC(4); break;
		}
		if ((op & 0xFFC0) == 0x4840 && mode >= 2) {
			u32 addr = ea_calc(_CPUCA mode, reg, 4);
			EA_COMMIT_POST();
			A[7] -= 4;
			wr32(_CPUCA A[7], addr);
			A7COM(7);
			CYC(12 + calc_cyc(mode));
			break;
		}
		if ((op & 0xFFF8) == 0x4880 && mode == 0) {
			D[op & 7] = (D[op & 7] & 0xFFFF0000) | ((u16)(i16)(i8)(D[op & 7] & 0xFF));
			SETNZ16((u16)D[op & 7]);
			CYC(4); break;
		}
		if ((op & 0xFFF8) == 0x48C0 && mode == 0) {
			D[op & 7] = (u32)(i16)(D[op & 7] & 0xFFFF);
			SETNZ32(D[op & 7]);
			CYC(4); break;
		}
		if ((op & OP_DIR) && sz_field <= 2) {
			u32 src = ea_read(_CPUCA mode, reg, 2);
			u16 dn_val = (u16)(D[rn] & 0xFFFF);
			flag_z = dn_val;
			flag_v = 0; flag_c = 0;
			if ((i16)dn_val < 0 || (i16)dn_val >(i16)(u16)src) {
				flag_n = ((u16)(dn_val) >> 15) << 7;
				exception_trap(VEC_CHK);
			}
			CYC(10); break;
		}
		if ((op & 0xFFC0) == 0x4800) {
			u32 ea_addr;
			u8 val = (u8)ea_read_save(_CPUCA mode, reg, 1, &ea_addr);
			u8 xc = flag_x ? 1 : 0;
			u8 bin_result = (u8)(0 - val - xc);
			int low_borrow = ((val & 0xF) + xc > 0);
			int borrow = (val + xc > 0);
			u8 result = bin_result;
			if (low_borrow) result -= 6;
			if (borrow) result -= 0x60;
			if (result) flag_z = result;
			flag_n = result & 0x80;
			flag_v = (bin_result & 0x80 && !(result & 0x80)) ? 0x80 : 0;
			int bcd_borrow = borrow || (low_borrow && (bin_result >> 4) == 0);
			SETCX(bcd_borrow);
			ea_write_saved(_CPUCA mode, reg, 1, result, ea_addr);
			CYC(6); break;
		}
		if ((op & 0xFF80) == 0x4880) {
			int szm = (op & 0x0040) ? 4 : 2;
			u16 rlist = F();
			int nreg = popcount16(rlist);
			if (mode == 4) {
				int i;
				int cnt = 0;
				for (i = 0; i < 16; i++) { if (rlist & (1 << i)) cnt++; }
				u32 dec = cnt * szm;
				SET_DFPC(PC - 2);

				u32 an_init = A[reg];

				u32 addr = an_init;
				for (i = 0; i < 16; i++) {
					if (rlist & (1 << i)) {
						u32 val;
						if (i >= 8) val = D[15 - i];
						else {
							int areg = 7 - i;
							val = (areg == reg) ? an_init : A[areg];
						}
						if (szm == 4) {
							addr -= 2;
							wr16(_CPUCA addr, val & 0xFFFF);
							addr -= 2;
							wr16(_CPUCA addr, (val >> 16) & 0xFFFF);
						}
						else {
							addr -= 2;
							wr16(_CPUCA addr, val & 0xFFFF);
						}
					}
				}
				A[reg] = an_init - dec;
			}
			else if (mode == 3) {
				u32 an_init = A[reg];
				int i, cnt = 0;
				for (i = 0; i < 16; i++) { if (rlist & (1 << i)) cnt++; }
				u32 addr = A[reg];
				SET_DFPC(PC - 2);
				for (i = 0; i < 16; i++) {
					if (rlist & (1 << i)) {
						u32 val;
						if (i < 8) val = D[i];
						else val = (i - 8 == reg) ? an_init : A[i - 8];
						if (szm == 4) {
							A[reg] += 2;
							if (reg == 7) A7COM(7);
							wr16(_CPUCA addr, (val >> 16) & 0xFFFF);
							addr += 2;
							A[reg] += 2;
							if (reg == 7) A7COM(7);
							wr16(_CPUCA addr, val & 0xFFFF);
							addr += 2;
						}
						else {
							A[reg] += 2;
							if (reg == 7) A7COM(7);
							wr16(_CPUCA addr, (val & 0xFFFF));
							addr += 2;
						}
					}
				}
				A[reg] = an_init + (u32)(cnt * szm);
			}
			else {
				u32 addr = ea_calc(_CPUCA mode, reg, szm);
				EA_COMMIT_POST();

				int i;
				for (i = 0; i < 16; i++) {
					if (rlist & (1 << i)) {
						if (szm == 4) wr32(_CPUCA addr, i < 8 ? D[i] : A[i - 8]);
						else wr16(_CPUCA addr, (i < 8 ? D[i] : A[i - 8]) & 0xFFFF);
						addr += szm;
					}
				}
			}
			CYC((szm == 4 ? 12 : 8) + 2 * nreg); break;
		}
		if ((op & 0xFF80) == 0x4C80) {
			int szm = (op & 0x0040) ? 4 : 2;
			u16 rlist = F();
			int nreg = popcount16(rlist);
			u32 addr;
			if (mode == 3) {
				u32 an_init = A[reg];
				int cnt = 0;
				int i;
				for (i = 0; i < 16; i++) { if (rlist & (1 << i)) cnt++; }
				addr = A[reg];
				SET_DFPC(PC - 2);
				for (i = 0; i < 16; i++) {
					if (rlist & (1 << i)) {
						if (szm == 4) {
							A[reg] += 2;
							u16 hi = rd16(_CPUCA addr);
							addr += 2;
							A[reg] += 2;
							u16 lo = rd16(_CPUCA addr);
							addr += 2;
							u32 v = ((u32)hi << 16) | lo;
							if (i < 8) D[i] = v; else A[i - 8] = v;
						}
						else {
							A[reg] += 2;
							u16 v = rd16(_CPUCA addr);
							addr += 2;
							if (i < 8) D[i] = (u32)(i16)v; else A[i - 8] = (u32)(i16)v;
						}
					}
				}
				A[reg] = an_init + (u32)(cnt * szm);
			}
			else {
				addr = ea_calc(_CPUCA mode, reg, szm);
				EA_COMMIT_POST();

				int i;
				for (i = 0; i < 16; i++) {
					if (rlist & (1 << i)) {
						u32 v = szm == 4 ? rd32(_CPUCA addr) : (u32)(i16)rd16(_CPUCA addr);
						if (i < 8) D[i] = v; else A[i - 8] = v;
						addr += szm;
					}
				}
			}
			CYC(12 + 2 * nreg); break;
		}
		if ((op & 0xF1C0) == 0x41C0) {
			A[rn] = ea_calc(_CPUCA mode, reg, 4);
			EA_COMMIT_POST();
			CYC(4 + calc_cyc(mode));
			break;
		}
		if ((op & 0xFF00) == 0x4E00) {
			u8 sub4e = op & 0xFF;
			if (sub4e == 0x71) { CYC(4); break; }
			if (sub4e == 0x75) { SET_DFPC(PC); PC = POP32(); PF_INVALIDATE(); CYC(16); break; }
			if ((sub4e & 0xF0) == 0x40) { exception_trap(VEC_TRAP_BASE + (sub4e & 0xF)); break; }
			if ((sub4e & 0xF8) == 0x50) {
				u8 an = sub4e & 7;
				i16 disp = (i16)F();
				SET_DFPC(PC);
				A[7] -= 4; A7COM(7); wr32(_CPUCA A[7], A[an]);
				A[an] = A[7];
				A[7] += (u32)(i32)disp;
				CYC(12);
				break;
			}
			if ((sub4e & 0xF8) == 0x58) {
				u8 an = sub4e & 7;
				A[7] = A[an];
				SET_DFPC(instr_addr + 2);
				A[an] = POP32();
				CYC(8); break;
			}
			if ((sub4e & 0xF8) == 0x60) {
				if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				USP = A[sub4e & 7]; CYC(4); break;
			}
			if ((sub4e & 0xF8) == 0x68) {
				if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				A[sub4e & 7] = USP; CYC(4); break;
			}
			if (sub4e == 0x70) {
				if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				CYC(128); break;
			}
			if (sub4e == 0x72) {
				if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				write_sr(_CPUCA F()); cpu->stopped = 1; CYC(4); break;
			}
			if (sub4e == 0x73) {
				if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
				SET_DFPC(PC);
				u16 new_sr = POP16(); u32 new_pc = POP32();
				write_sr(_CPUCA new_sr); PC = new_pc; PF_INVALIDATE(); CYC(20); break;
			}
			if (sub4e == 0x76) {
				if (flag_v) { exception_trap(VEC_TRAPV); }
				CYC(4); break;
			}
			if (sub4e == 0x77) {
				SET_DFPC(PC);
				u16 ccr_word = POP16();
				u16 sr = get_sr(_CPUPA);
				sr = (sr & ~CCR_MASK) | (ccr_word & CCR_MASK);
				set_sr_flags(_CPUCA sr);
				SR = (SR & ~CCR_MASK) | (sr & CCR_MASK);
				PC = POP32(); PF_INVALIDATE(); CYC(20); break;
			}
			if ((op & 0xFFC0) == 0x4E80) {
				u32 addr = ea_calc(_CPUCA mode, reg, 4);
				EA_COMMIT_POST(); SET_DFPC(PC);
				PUSH32(PC); PC = addr; PF_INVALIDATE();
			CYC(16 + calc_cyc(mode));
			break;
			}
			if ((op & 0xFFC0) == 0x4EC0) {
				u32 addr = ea_calc(_CPUCA mode, reg, 4);
				EA_COMMIT_POST(); PC = addr; PF_INVALIDATE();
			CYC(4 + calc_cyc(mode));
			break;
			}
			CYC(4); break;
		}
		if ((op & 0xFFC0) == 0x40C0) {
			u32 msr_ea;
			(void)ea_read_save(_CPUCA mode, reg, 2, &msr_ea);
			ea_write_saved(_CPUCA mode, reg, 2, get_sr(_CPUPA), msr_ea);
			CYC(8); break;
		}
		if ((op & 0xFFC0) == 0x44C0) {
			u16 val = (u16)ea_read(_CPUCA mode, reg, 2);
			u16 sr = get_sr(_CPUPA);
			sr = (sr & ~CCR_MASK) | (val & CCR_MASK);
			set_sr_flags(_CPUCA sr);
			SR = (SR & ~CCR_MASK) | (sr & CCR_MASK);
			CYC(12); break;
		}
		if ((op & 0xFFC0) == 0x46C0) {
			if (!supervisor(_CPUPA)) { exception_fault(VEC_PRIV); break; }
			write_sr(_CPUCA(u16)ea_read(_CPUCA mode, reg, 2));
			CYC(12); break;
		}
		switch (rn) {
		case 0: {
			if (real_sz == 0) { CYC(4); break; }
			u32 ea_addr;
			u32 val = ea_read_save(_CPUCA mode, reg, real_sz, &ea_addr);
			u32 result;
			DO_NEGX_SZ(real_sz, val, result);
			ea_write_saved(_CPUCA mode, reg, real_sz, result, ea_addr);
			CYC(mode == 0 ? 4 : (real_sz == 4 ? 12 : 8));
			break;
		}
		case 1: {
			if (real_sz == 0) { CYC(4); break; }
			u32 clr_ea;
			(void)ea_read_save(_CPUCA mode, reg, real_sz, &clr_ea);
			ea_write_saved(_CPUCA mode, reg, real_sz, 0, clr_ea);
			SETNZ_SZ(real_sz, 0);
			CYC(4); break;
		}
		case 2: {
			if (real_sz == 0) { CYC(4); break; }
			u32 ea_addr;
			u32 val = ea_read_save(_CPUCA mode, reg, real_sz, &ea_addr);
			{ u32 res; DO_SUB_SZ(real_sz, 0, val, res); ea_write_saved(_CPUCA mode, reg, real_sz, res, ea_addr); }
			CYC(4); break;
		}
		case 3: {
			if (real_sz == 0) { CYC(4); break; }
			u32 ea_addr;
			u32 val = ea_read_save(_CPUCA mode, reg, real_sz, &ea_addr);
			val = ~val;
			if (real_sz == 1) val &= 0xFF; else if (real_sz == 2) val &= 0xFFFF; SETNZ_SZ(real_sz, val);
			ea_write_saved(_CPUCA mode, reg, real_sz, val, ea_addr);
			CYC(4); break;
		}
		case 5:
			if (sz_field == 3) {
				u32 ea_addr;
				u32 val = ea_read_save(_CPUCA mode, reg, 1, &ea_addr);
				flag_n = val & 0x80;
				flag_z = (u8)val;
				flag_v = 0; flag_c = 0;
				ea_write_saved(_CPUCA mode, reg, 1, val | 0x80, ea_addr);
				CYC(4); break;
			}
			{
				if (real_sz == 0) { CYC(4); break; }
				u32 val = ea_read(_CPUCA mode, reg, real_sz);
				SETNZ_SZ(real_sz, val);
				CYC(4); break;
			}
		default: CYC(4); break;
		}
		break;
	}
	case 0x05: {
		u8 sz_field = (op >> 6) & 3;
		if (sz_field == 3 && mode == 1) {
			u8 cond = (op >> 8) & 0x0F;
			u8 dn = reg;

			u32 disp_base = PC;
			i16 disp = (i16)F();
			if (!cc(_CPUCA cond)) {
				u32 orig = D[dn];
				u16 count = (u16)(D[dn] & 0xFFFF);
				count--;
				D[dn] = (D[dn] & 0xFFFF0000) | (count & 0xFFFF);
				if (count != 0xFFFF) {
#ifndef FAST_MEM
					dbcc_pending_reg = dn;
					dbcc_pending_val = orig;
#endif
					PC = disp_base + (u32)(i32)disp; PF_INVALIDATE(); CYC(10);
				}
				else CYC(14);
			}
			else CYC(12);
			break;
		}
		if (sz_field == 3) {
			u8 cond = (op >> 8) & 0x0F;
			u8 val = cc(_CPUCA cond) ? SCC_TRUE : SCC_FALSE;
			u32 scc_ea;
			(void)ea_read_save(_CPUCA mode, reg, 1, &scc_ea);
			ea_write_saved(_CPUCA mode, reg, 1, val, scc_ea);
			CYC(4); break;
		}
		u8 data = (op >> 9) & 7; if (data == 0) data = IMM_ZERO_VAL;
		u8 sub = (op >> 8) & 1;
		if (mode == 1) {
			if (sub == 0) A[reg] += data;
			else A[reg] -= data;
			CYC(4); break;
		}
		int real_sz = DECODE_SZ(sz_field);
		u32 ea_addr;
		u32 val = ea_read_save(_CPUCA mode, reg, real_sz, &ea_addr);
		u32 result;
		if (sub == 0) {
			DO_ADD_SZ(real_sz, val, data, result);
		}
		else {
			DO_SUB_SZ(real_sz, val, data, result);
		}
		ea_write_saved(_CPUCA mode, reg, real_sz, result, ea_addr);
		CYC(mode == 0 ? 4 : (real_sz == 4 ? 12 : 8));
		 break;
	}
	case 0x06: {
		i8 disp8 = op & 0xFF;
		i32 disp;

		u32 disp_base = PC;
		if (disp8 == 0) disp = (i16)F();
		else disp = (i32)disp8;
		u32 pc_after = PC;
		u8 cond = (op >> 8) & 0x0F;
		if (cond == 0) { PC = disp_base + disp; PF_INVALIDATE(); CYC(10); break; }
		if (cond == 1) { SET_DFPC(PC); PUSH32(pc_after); PC = disp_base + disp; PF_INVALIDATE(); CYC(18); break; }
		if (cc(_CPUCA cond)) { PC = disp_base + disp; PF_INVALIDATE(); CYC(10); }
		else CYC(8);
		break;
	}
	case 0x07: {
		i8 imm = op & 0xFF;
		D[(op >> 9) & 7] = (u32)(i32)imm;
		SETNZ32(D[(op >> 9) & 7]);
		CYC(4); break;
	}
	case 0x08: {
		u8 sz_field = (op >> 6) & 3;
		u8 rn = (op >> 9) & 7;
		if (sz_field == 3) {
			u16 divisor = (u16)ea_read(_CPUCA mode, reg, 2);
			if (divisor == 0) {
				flag_n = 0; flag_z = 1; flag_c = 0;
				exception_common(_CPUCA VEC_DIV0, instr_addr); break;
			}
			u32 dividend = D[rn];
			if (op & OP_DIR) {
				i32 q = (i32)dividend / (i16)(i32)(i16)divisor;
				i32 r = (i32)dividend % (i16)(i32)(i16)divisor;
				if (q > 32767 || q < -32768) {
					flag_v = 0x80; flag_c = 0;
					break;
				}
				D[rn] = ((u32)(u16)q) | ((u32)((u16)(r & 0xFFFF)) << 16);
				flag_n = (i16)q < 0 ? 0x80 : 0;
				flag_z = (u16)q;
				flag_v = 0; flag_c = 0;
			}
			else {
				u32 q = dividend / divisor;
				u32 r = dividend % divisor;
				if (q > 0xFFFF) {
					flag_v = 0x80; flag_c = 0;
					break;
				}
				D[rn] = (q & 0xFFFF) | ((r & 0xFFFF) << 16);
				flag_n = ((u16)(q) >> 15) << 7;
				flag_z = (u16)q;
				flag_v = 0; flag_c = 0;
			}
			CYC(140); break;
		}
		if (sz_field == 0 && (op & OP_DIR) && (mode == 0 || mode == 1)) {
			do_bcd(_CPUCA 0, rn, mode, op & 7);
			CYC(6); break;
		}
		int real_sz = DECODE_SZ(sz_field);
		u32 result;
		ALU_LOGICAL(op, real_sz, rn, mode, reg, result);
		break;
	}
	case 0x09: {
		u8 sz_field = (op >> 6) & 3;
		u8 rn = (op >> 9) & 7;
		if (sz_field == 3) {
			int sz = (op & OP_DIR) ? 4 : 2;
			u32 val = ea_read(_CPUCA mode, reg, sz);
			if (sz == 2) val = (u32)(i16)(val & 0xFFFF);
			A[rn] -= val;
			CYC(8 + (mode == 7 ? ea_cyc7(reg, sz) : ea_cyc(mode, sz)));
			break;
		}
	if ((mode == 0 || mode == 1) && (op & OP_DIR)) {
			do_addx_subx(_CPUCA 0, DECODE_SZ(sz_field), rn, mode, op & 7);
			CYC(4); break;
		}
		int real_sz = DECODE_SZ(sz_field);
		u32 result;
		ALU_ARITH(DO_SUB_SZ, real_sz, op, rn, mode, reg, result);
		break;
	}
	case 0x0A: exception_fault(VEC_LINEA); break;
	case 0x0B: {
		u8 sz_field = (op >> 6) & 3;
		u8 rn = (op >> 9) & 7;
		if (sz_field == 3) {
			int sz = (op & OP_DIR) ? 4 : 2;
			u32 val = ea_read(_CPUCA mode, reg, sz);
			if (sz == 2) val = (u32)(i16)(val & 0xFFFF);
			u32 dst = A[rn];
			uint64_t r = (uint64_t)dst - val;
			SETCMP32(dst, val, r);
			CYC(6); break;
		}
		int real_sz = DECODE_SZ(sz_field);
		if (mode == 1 && !(op & OP_DIR)) {
			u32 val = A[reg];
			u32 dst = D[rn];
			DO_CMP_SZ(real_sz, dst, val);
			CYC(6); break;
		}
		if (real_sz == 0) { CYC(4); break; }
		if (op & OP_DIR) {
			if (mode == 1) {
				u32 src = ea_read(_CPUCA 3, reg, real_sz);
				u32 dst = ea_read(_CPUCA 3, rn, real_sz);
				DO_CMP_SZ(real_sz, dst, src);
				CYC(real_sz == 4 ? 6 : 4); break;
			}
			u32 val, addr = 0; EA_DN_READ(val, addr, mode, reg, real_sz);
			u32 result = D[rn] ^ val;
			EA_DN_WRITE(reg, real_sz, result, addr, mode);
			SETNZ_SZ(real_sz, result);
			if (mode == 0) CYC(real_sz == 4 ? 8 : 4);
			else CYC((real_sz == 4 ? 12 : 8) + (mode == 7 ? ea_cyc7(reg, real_sz) : ea_cyc(mode, real_sz)));
			break;
		}
		else {
			u32 val = ea_read(_CPUCA mode, reg, real_sz);
			u32 dst = D[rn];
			DO_CMP_SZ(real_sz, dst, val);
			CYC(real_sz == 4 ? (mode == 0 ? 8 : 6) : 4);
			break;
		}
	}
	case 0x0C: {
		u8 sz_field = (op >> 6) & 3;
		u8 rn = (op >> 9) & 7;
		if (sz_field == 3) {
			u16 val = (u16)ea_read(_CPUCA mode, reg, 2);
			if (op & OP_DIR) { D[rn] = (u32)((i32)(i16)(D[rn] & 0xFFFF) * (i32)(i16)val); }
			else { D[rn] = (u32)(D[rn] & 0xFFFF) * (u32)val; }
			SETNZ32(D[rn]);
			CYC(28); break;
		}
		if ((op & 0xF130) == 0xC100) {
			u8 opmode = (op >> 3) & 0x1F;
			if (opmode == 0x08 || opmode == 0x09 || opmode == 0x11) {
				u32 t;
				if (opmode == 0x08) { t = D[rn]; D[rn] = D[reg]; D[reg] = t; }
				else if (opmode == 0x09) { t = A[rn]; A[rn] = A[reg]; A[reg] = t; }
				else { t = D[rn]; D[rn] = A[reg]; A[reg] = t; }
				CYC(6); break;
			}
		}
		if (sz_field == 0 && (op & OP_DIR) && (mode == 0 || mode == 1)) {
			do_bcd(_CPUCA 1, rn, mode, op & 7);
			CYC(6); break;
		}
		int real_sz = DECODE_SZ(sz_field);
		u32 result;
		ALU_LOGICAL_AND(op, real_sz, rn, mode, reg, result);
		break;
	}
	case 0x0D: {
		u8 sz_field = (op >> 6) & 3;
		u8 rn = (op >> 9) & 7;
		if (sz_field == 3) {
			int sz = (op & OP_DIR) ? 4 : 2;
			u32 val = ea_read(_CPUCA mode, reg, sz);
			if (sz == 2) val = (u32)(i16)(val & 0xFFFF);
			A[rn] += val;
			CYC(8); break;
		}
		if ((mode == 0 || mode == 1) && (op & OP_DIR)) {
			do_addx_subx(_CPUCA 1, DECODE_SZ(sz_field), rn, mode, op & 7);
			CYC(4); break;
		}
		int real_sz = DECODE_SZ(sz_field);
		if (real_sz == 0) { CYC(4); break; }
		u32 result;
		ALU_ARITH(DO_ADD_SZ, real_sz, op, rn, mode, reg, result);
	break;
	}
	case 0x0E: {
		static const int reg_sub[2][4] = {
		{ 1, 3, 7, 5 },
		{ 0, 2, 6, 4 },
		};
		u8 sz_field = (op >> 6) & 3;
		if (sz_field == 3) {
			int ttt = (op >> 9) & 7;
			int d = (op >> 8) & 1;
			int sub = reg_sub[d][ttt & 3];
			u32 addr = ea_calc(_CPUCA mode, reg, 2);
			u16 val = rd16(_CPUCA addr);
			EA_COMMIT_POST();
			val = (u16)shift_rot(_CPUCA sub, val, 1, 2);
			wr16(_CPUCA addr, val);
			CYC(8 + ea_cyc(mode, 2));
			break;
		}
		int dir = (op >> 8) & 1;
		int type = (op >> 3) & 3;
		int sub = reg_sub[dir][type];
		int cnt_reg = (op >> 5) & 1;
		int real_sz = DECODE_SZ(sz_field);
		u8 rn_shift = op & 7;
		int cnt;
		if (cnt_reg) cnt = D[(op >> 9) & 7] & SHIFT_CNT_MASK;
		else { cnt = (op >> 9) & 7; if (cnt == 0) cnt = IMM_ZERO_VAL; }
		u32 val = D[rn_shift];
		val = shift_rot(_CPUCA sub, val, cnt, real_sz);
		DN_WRITE(rn_shift, real_sz, val);
		CYC((real_sz == 4 ? 8 : 6) + cnt * 2);
		break;
	}
	case 0x0F:
		exception_fault(VEC_LINEF);
		break;
	default:
		exception_fault(VEC_ILLEGAL);
		break;
	}
	if (supervisor(_CPUPA)) SR |= SR_S;
#ifndef FAST_MEM
	check_align = 0;
#endif

	if (PC & 1) {
		int is_jsr = ((op & 0xFFC0) == 0x4E80);
		if (is_jsr) {
			A[7] = sp_at_step_start;
		}
		if (ea_pending_dec_reg >= 0) {
			A[ea_pending_dec_reg] += ea_pending_dec_val;
			A7COM(ea_pending_dec_reg);
			ea_pending_dec_reg = -1;
		}
		dbcc_pending_reg = -1;
		addr_error_exception(_CPUCA PC, PC - 4, op, 1, 1);
	}

	/* Flag-based AE recovery: check if an address error fired during execution. I... */
	if (UNLIKELY(addr_error_fired)) {
		A[7] = sp_at_step_start;
		if (ea_pending_dec_reg >= 0) {
			A[ea_pending_dec_reg] += ea_pending_dec_val;
			ea_pending_dec_reg = -1;
		}
#ifndef FAST_MEM
		if (dbcc_pending_reg >= 0) {
			D[dbcc_pending_reg] = (D[dbcc_pending_reg] & 0xFFFF0000) | (dbcc_pending_val & 0xFFFF);
			dbcc_pending_reg = -1;
		}
#else
		/* FAST_MEM: SET_DFPC/SET_FIP are no-ops, so data_fault_pc/ir were never set. ... */
		data_fault_pc = instr_addr + 2;
		data_fault_ir = op;
		addr_error_fired = 0; /* allow AE handler stack pushes */
#endif
		addr_error_exception(_CPUCA fault_addr, data_fault_pc,
			data_fault_ir, fault_is_read, 0);
		return STEPRET;
	}
#ifndef FAST_MEM
	prev_sp = sp_at_step_start;
	prev_opcode = op;
#endif
	return STEPRET;
}

#ifdef SINGLE_INST
CPU68000* cpu68000_get(void) {
	memcpy(g_cpu.D, D, sizeof(D)); memcpy(g_cpu.A, A, sizeof(A));
	g_cpu.USP = USP; g_cpu.PC = PC; g_cpu.SR = SR;
	g_cpu.flag_n = flag_n; g_cpu.flag_z = flag_z; g_cpu.flag_v = flag_v;
	g_cpu.flag_c = flag_c; g_cpu.flag_x = flag_x;
	g_cpu.ram = ram; g_cpu.mem_read = mem_read; g_cpu.mem_write = mem_write;
	g_cpu.int_ack = int_ack;
	if (!g_cpu.fault) g_cpu.fault = (CPU68000Fault*)calloc(1, sizeof(CPU68000Fault));
	*g_cpu.fault = fault;
	return &g_cpu;
}
void cpu68000_set(const CPU68000* c) {
	memcpy(D, c->D, sizeof(D)); memcpy(A, c->A, sizeof(A));
	USP = c->USP; PC = c->PC; SR = c->SR;
	flag_n = c->flag_n; flag_z = c->flag_z; flag_v = c->flag_v;
	flag_c = c->flag_c; flag_x = c->flag_x;
	ram = c->ram; mem_read = c->mem_read; mem_write = c->mem_write;
	int_ack = c->int_ack;
	if (c->fault) fault = *c->fault;
}

#endif

#ifdef FAST_MEM
#include "cpu68000_run.c"
#endif
