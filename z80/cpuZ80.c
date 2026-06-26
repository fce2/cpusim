#ifdef DEBUG
	#include <stdio.h>
#endif
#include "cpuZ80.h"

/* Internal constants (not exposed in .h) */
#define Z80_H16		0x1000	/* half-carry bit for 16-bit add/sub */
#define Z80_S16		0x8000	/* sign bit for 16-bit result */
#define Z80_C16		0x10000	/* carry-out for 16-bit add/sub */
#define INT_ADDR	0x0038	/* IM 0/1 interrupt vector */
#define NMI_ADDR	0x0066	/* NMI vector */

/* Parity and SZP flags: precomputed 256-byte table. SZYXP for each byte value 0-255.
 * SF=0x80 ZF=0x40 YF=0x20 HF=0x10 XF=0x08 PF=0x04 NF=0x02 CF=0x01
 * Table entry = SF|YF|XF from value, ZF if value==0, PF if even parity. No HF/NF/CF. */
#ifndef NO_SZP_TABLE
	static const u8 szp_table[256] = {
		0x44,0x00,0x00,0x04,0x00,0x04,0x04,0x00,0x08,0x0C,0x0C,0x08,0x0C,0x08,0x08,0x0C,0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04,0x0C,0x08,0x08,0x0C,0x08,0x0C,0x0C,0x08,
		0x20,0x24,0x24,0x20,0x24,0x20,0x20,0x24,0x2C,0x28,0x28,0x2C,0x28,0x2C,0x2C,0x28,0x24,0x20,0x20,0x24,0x20,0x24,0x24,0x20,0x28,0x2C,0x2C,0x28,0x2C,0x28,0x28,0x2C,
		0x00,0x04,0x04,0x00,0x04,0x00,0x00,0x04,0x0C,0x08,0x08,0x0C,0x08,0x0C,0x0C,0x08,0x04,0x00,0x00,0x04,0x00,0x04,0x04,0x00,0x08,0x0C,0x0C,0x08,0x0C,0x08,0x08,0x0C,
		0x24,0x20,0x20,0x24,0x20,0x24,0x24,0x20,0x28,0x2C,0x2C,0x28,0x2C,0x28,0x28,0x2C,0x20,0x24,0x24,0x20,0x24,0x20,0x20,0x24,0x2C,0x28,0x28,0x2C,0x28,0x2C,0x2C,0x28,
		0x80,0x84,0x84,0x80,0x84,0x80,0x80,0x84,0x8C,0x88,0x88,0x8C,0x88,0x8C,0x8C,0x88,0x84,0x80,0x80,0x84,0x80,0x84,0x84,0x80,0x88,0x8C,0x8C,0x88,0x8C,0x88,0x88,0x8C,
		0xA4,0xA0,0xA0,0xA4,0xA0,0xA4,0xA4,0xA0,0xA8,0xAC,0xAC,0xA8,0xAC,0xA8,0xA8,0xAC,0xA0,0xA4,0xA4,0xA0,0xA4,0xA0,0xA0,0xA4,0xAC,0xA8,0xA8,0xAC,0xA8,0xAC,0xAC,0xA8,
		0x84,0x80,0x80,0x84,0x80,0x84,0x84,0x80,0x88,0x8C,0x8C,0x88,0x8C,0x88,0x88,0x8C,0x80,0x84,0x84,0x80,0x84,0x80,0x80,0x84,0x8C,0x88,0x88,0x8C,0x88,0x8C,0x8C,0x88,
		0xA0,0xA4,0xA4,0xA0,0xA4,0xA0,0xA0,0xA4,0xAC,0xA8,0xA8,0xAC,0xA8,0xAC,0xAC,0xA8,0xA4,0xA0,0xA0,0xA4,0xA0,0xA4,0xA4,0xA0,0xA8,0xAC,0xAC,0xA8,0xAC,0xA8,0xA8,0xAC,
	};
	#define SZP_FLAGS(v) szp_table[(v)]
	#define PARITY(v) ((szp_table[(v)] & Z80_PF) ? Z80_PF : 0)
#else
	/* Computed SZP flags (no table). Slower but saves 256 bytes. */
	INLINE u8 parity8(u8 v) { v ^= v >> 4; return ((0x9669 >> (v & 0xF)) & 1) * Z80_PF; }
	#define PARITY(v) parity8(v)
	#define SZP_FLAGS(v) ((u8)((v) & (Z80_SF|Z80_YF|Z80_XF)) | ((v) ? 0 : Z80_ZF) | parity8(v))
#endif

/* SINGLE_INST: bare static globals. Struct mode: field macros */
#ifdef SINGLE_INST
	static Reg16 rAF, rBC, rDE, rHL, rAF_, rBC_, rDE_, rHL_;
	static Reg16 rIX, rIY, rWZ, rSP, rPC;
	static u8 I, R, IFF1, IFF2, IM, Q;
	static u8* ram;
	static cpuZ80_in_cb port_in;
	static cpuZ80_out_cb port_out;
	#ifdef MEM_IO
		static cpuZ80_read_cb mem_read;
		static cpuZ80_write_cb mem_write;
	#endif
	/* Register aliases — SINGLE_INST maps to static globals */
	#define R8(n,g) n
	#define R16(n,g) n
#else
	#define R8(n,g) g
	#define R16(n,g) g
#endif
#define A				R8(rAF.b.h,		cpu->af.b.h)
#define F				R8(rAF.b.l,		cpu->af.b.l)
#define B				R8(rBC.b.h,		cpu->bc.b.h)
#define C				R8(rBC.b.l,		cpu->bc.b.l)
#define D				R8(rDE.b.h,		cpu->de.b.h)
#define E				R8(rDE.b.l,		cpu->de.b.l)
#define H				R8(rHL.b.h,		cpu->hl.b.h)
#define L				R8(rHL.b.l,		cpu->hl.b.l)
#define A_				R8(rAF_.b.h,	cpu->af_.b.h)
#define F_				R8(rAF_.b.l,	cpu->af_.b.l)
#define B_				R8(rBC_.b.h,	cpu->bc_.b.h)
#define C_				R8(rBC_.b.l,	cpu->bc_.b.l)
#define D_				R8(rDE_.b.h,	cpu->de_.b.h)
#define E_				R8(rDE_.b.l,	cpu->de_.b.l)
#define H_				R8(rHL_.b.h,	cpu->hl_.b.h)
#define L_				R8(rHL_.b.l,	cpu->hl_.b.l)
#define AF				R16(rAF.w,		cpu->af.w)
#define BC				R16(rBC.w,		cpu->bc.w)
#define DE				R16(rDE.w,		cpu->de.w)
#define HL				R16(rHL.w,		cpu->hl.w)
#define AF_				R16(rAF_.w,		cpu->af_.w)
#define BC_				R16(rBC_.w,		cpu->bc_.w)
#define DE_				R16(rDE_.w,		cpu->de_.w)
#define HL_				R16(rHL_.w,		cpu->hl_.w)
#define IX				R16(rIX.w,		cpu->ix.w)
#define IY				R16(rIY.w,		cpu->iy.w)
#define WZ				R16(rWZ.w,		cpu->wz.w)
#define SP				R16(rSP.w,		cpu->sp.w)
#define PC				R16(rPC.w,		cpu->pc.w)
#define I				R8(I,			cpu->I)
#define R				R8(R,			cpu->R)
#define IFF1			R8(IFF1,		cpu->IFF1)
#define IFF2			R8(IFF2,		cpu->IFF2)
#define IM				R8(IM,			cpu->IM)
#define Q				R8(Q,			cpu->Q)
#define ram				R8(ram,			cpu->ram)
#define port_in			R8(port_in,		cpu->port_in)
#define port_out		R8(port_out,	cpu->port_out)
#ifdef MEM_IO
	#define mem_read	R8(mem_read,	cpu->mem_read)
	#define mem_write	R8(mem_write,	cpu->mem_write)
#endif

#if defined(MEM_IO) && !defined(NO_IO_MAP)
	static u8 io_map[256]; /* per-page bitmap: 0=RAM, nonzero=callback */
#endif

/* IO_RD/IO_WR: RAM-only (no MEM_IO), always-callback (NO_IO_MAP), or io_map check */
#if !defined(MEM_IO)
	#define IO_RD(a)	ram[a]
	#define IO_WR(a,v)	ram[a] = v
#elif defined(NO_IO_MAP)
	#define IO_RD(a)	mem_read(_CPUCA a)
	#define IO_WR(a,v)	mem_write(_CPUCA a, v)
#elif defined(SINGLE_INST)
	#define IO_RD(a)	(io_map[(a) >> 8] ? mem_read(a) : ram[a])
	#define IO_WR(a,v)	do { if(io_map[(a) >> 8]) mem_write(a, v); else ram[a] = v; } while(0)
#else
	#define IO_RD(a)	(io_map[(a) >> 8] ? mem_read(cpu, a) : ram[a])
	#define IO_WR(a,v)	do { if(io_map[(a) >> 8]) mem_write(cpu, a, v); else ram[a] = v; } while(0)
#endif

/* Port I/O: defaults installed in init (no NULL check needed) */
#ifdef SINGLE_INST
	#define IO_PIN(p)		port_in(p)
	#define IO_POUT(p,v)	port_out(p, v)
#else
	#define IO_PIN(p)		port_in(cpu, p)
	#define IO_POUT(p,v)	port_out(cpu, p, v)
#endif

/* Fetch, register access, and instruction macros */
#define FB()	(ram[PC++])
#define F16()	(PC += 2, (u16)ram[(u16)(PC-2)] | ((u16)ram[(u16)(PC-1)] << 8))
#define INC_R()	do { R = (R & 0x80) | ((R + 1) & 0x7F); } while(0)
/* Register pair macros — zero-shift via Reg16 union */
#define SET_AF(v)	(AF = (v))
#define SET_BC(v)	(BC = (v))
#define SET_DE(v)	(DE = (v))
#define SET_HL(v)	(HL = (v))

/* Swap macros: need whole-Reg16 access, not .w */
#ifdef SINGLE_INST
	#define EX_AFB()	do { Reg16 t_=rAF; rAF=rAF_; rAF_=t_; } while(0)
	#define EXX()		do { Reg16 t_; t_=rBC; rBC=rBC_; rBC_=t_; t_=rDE; rDE=rDE_; rDE_=t_; t_=rHL; rHL=rHL_; rHL_=t_; } while(0)
#else
	#define EX_AFB()	do { Reg16 t_=cpu->af; cpu->af=cpu->af_; cpu->af_=t_; } while(0)
	#define EXX()		do { Reg16 t_; t_=cpu->bc; cpu->bc=cpu->bc_; cpu->bc_=t_; t_=cpu->de; cpu->de=cpu->de_; cpu->de_=t_; t_=cpu->hl; cpu->hl=cpu->hl_; cpu->hl_=t_; } while(0)
#endif
#define YX_FLAGS(q) ((q) ? (A & (Z80_YF | Z80_XF)) : ((A | F) & (Z80_YF | Z80_XF)))
#define ROT_CB_FLAGS(r, v, y) SZP_FLAGS(r) | (((y) & 1) ? ((v) & 0x01 ? Z80_CF : 0) : ((v) & 0x80 ? Z80_CF : 0))
#define SET_XY(v) do { if(is_iy) IY = (v); else IX = (v); } while(0)

/* Shared instruction macros — single-line case bodies (3 copies each) */
#define DAA_INST() do { \
	u8 _add = 0, _cy = (F & Z80_CF) ? 1 : 0; \
	if((F & Z80_HF) || (A & 0x0F) > 9) _add = 6; \
	if(_cy || A > 0x99) _add |= 0x60; \
	if(A > 0x99) _cy = 1; \
	u8 _hf; \
	if(F & Z80_NF) { _hf = (A & 0x0F) < (_add & 0x0F) ? Z80_HF : 0; A -= _add; } \
	else { _hf = ((A & 0x0F) + (_add & 0x0F)) > 15 ? Z80_HF : 0; A += _add; } \
	F = _hf | SZP_FLAGS(A) | (_cy ? Z80_CF : 0) | (F & Z80_NF); Q = F; \
} while(0)
#define ROT_A_INST(_y) do { \
	u8 _cy = ((_y) & 1) ? ((A & 0x01) ? Z80_CF : 0) : ((A & 0x80) ? Z80_CF : 0); \
	A = rotate8(_CPUCA (_y), A); \
	F = (F & (Z80_SF | Z80_ZF | Z80_PF)) | (A & (Z80_YF | Z80_XF)) | _cy; Q = F; \
} while(0)
#define CPL_INST() do { \
	A = ~A; F = (F & (Z80_SF | Z80_ZF | Z80_PF | Z80_CF)) | Z80_HF | Z80_NF | (A & (Z80_YF | Z80_XF)); Q = F; \
} while(0)
#define SCF_INST(_pq) do { \
	F = (F & (Z80_SF | Z80_ZF | Z80_PF)) | Z80_CF | YX_FLAGS(_pq); Q = F; \
} while(0)
#define CCF_INST(_pq) do { \
	u8 _oc = F & Z80_CF; \
	F = (F & (Z80_SF | Z80_ZF | Z80_PF)) | (_oc ? Z80_HF : 0) | YX_FLAGS(_pq); \
	if(!_oc) F |= Z80_CF; Q = F; \
} while(0)

/* Data access and stack operations */
INLINE u8 rd(_CPUC u16 a) { return IO_RD(a); }
INLINE void wr(_CPUC u16 a, u8 v) { IO_WR(a, v); }
INLINE u8 pin(_CPUC u16 p) { return IO_PIN(p); }
INLINE void pout(_CPUC u16 p, u8 v) { IO_POUT(p, v); }
INLINE void push16(_CPUC u16 v) { SP--; wr(_CPUCA SP, v >> 8); SP--; wr(_CPUCA SP, v & 0xFF); }
INLINE u16 pop16(_CPUP) { u8 lo = rd(_CPUCA SP); SP++; u8 hi = rd(_CPUCA SP); SP++; return lo | ((u16)hi << 8); }

/* Register access — switch in both modes (offset tables were slower) */
INLINE u8 get_r(_CPUC int r) {
	switch (r) {
	case 0: return B; case 1: return C; case 2: return D; case 3: return E;
	case 4: return H; case 5: return L; case 6: return rd(_CPUCA HL); case 7: return A;
	} return 0;
}
INLINE void put_r(_CPUC int r, u8 v) {
	switch (r) {
	case 0: B = v; break; case 1: C = v; break; case 2: D = v; break; case 3: E = v; break;
	case 4: H = v; break; case 5: L = v; break; case 6: wr(_CPUCA HL, v); break; case 7: A = v; break;
	}
}
INLINE u8 get_r_ix(_CPUC int r, u16 ix) {
	switch (r) {
	case 0: return B; case 1: return C; case 2: return D; case 3: return E;
	case 4: return (u8)(ix >> 8); case 5: return (u8)(ix & 0xFF);
	case 6: { i8 d = (i8)FB(); return rd(_CPUCA ix + d); } case 7: return A;
	} return 0;
}
INLINE void put_r_ix(_CPUC int r, u8 v, int is_iy) {
	switch (r) {
	case 0: B = v; break; case 1: C = v; break; case 2: D = v; break; case 3: E = v; break;
	case 4: if (is_iy) IY = (v << 8) | (IY & 0xFF); else IX = (v << 8) | (IX & 0xFF); break;
	case 5: if (is_iy) IY = (IY & 0xFF00) | v; else IX = (IX & 0xFF00) | v; break;
	case 7: A = v; break;
	}
}

/* rp: 0=BC 1=DE 2=HL 3=SP(af=0)/AF(af=1) */
INLINE u16 get_rp(_CPUC int p, int af) {
	switch (p) { case 0: return BC; case 1: return DE; case 2: return HL; case 3: return af ? AF : SP; } return 0;
}
INLINE void set_rp(_CPUC int p, u16 v, int af) {
	switch (p) {
	case 0: SET_BC(v); break; case 1: SET_DE(v); break;
	case 2: SET_HL(v); break; case 3: if (af) SET_AF(v); else SP = v; break;
	}
}

/* cc(y): NZ=0 Z=1 NC=2 C=3 PO=4 PE=5 P=6 M=7. Even=flag clear, odd=flag set */
INLINE BOOL cc(_CPUC int y) {
	static const u8 cc_bit[] = { Z80_ZF, Z80_ZF, Z80_CF, Z80_CF, Z80_PF, Z80_PF, Z80_SF, Z80_SF };
	return (y & 1) ? (F & cc_bit[y]) != 0 : !(F & cc_bit[y]);
}

/* ALU operations */
INLINE void alu(_CPUC int op, u8 val) {
	u16 result; u8 r8, a = A, f = F;
	switch (op) {
	case 0: case 1: /* ADD/ADC */
		result = a + val + (op ? (f & Z80_CF) : 0); r8 = result;
		f = (r8 & (Z80_SF | Z80_YF | Z80_XF)) | (r8 ? 0 : Z80_ZF) | ((a ^ val ^ result) & Z80_HF) |
			(((a ^ r8) & (val ^ r8) & 0x80) ? Z80_PF : 0) | (result & 0x100 ? Z80_CF : 0);
		a = r8; break;
	case 2: case 3: /* SUB/SBC */
		result = a - val - (op & 1 ? (f & Z80_CF) : 0); r8 = result;
		f = Z80_NF | (r8 & (Z80_SF | Z80_YF | Z80_XF)) | (r8 ? 0 : Z80_ZF) | ((a ^ val ^ result) & Z80_HF) |
			(((a ^ val) & (a ^ r8) & 0x80) ? Z80_PF : 0) | (result & 0x100 ? Z80_CF : 0);
		a = r8; break;
	case 4: a &= val; f = Z80_HF | SZP_FLAGS(a); break;
	case 5: a ^= val; f = SZP_FLAGS(a); break;
	case 6: a |= val; f = SZP_FLAGS(a); break;
	case 7: /* CP - YF/XF from operand, not result */
		result = a - val; r8 = result;
		f = Z80_NF | (r8 & Z80_SF) | (val & (Z80_YF | Z80_XF)) | (r8 ? 0 : Z80_ZF) | ((a ^ val ^ result) & Z80_HF) |
			(((a ^ val) & (a ^ r8) & 0x80) ? Z80_PF : 0) | (result & 0x100 ? Z80_CF : 0); break;
	}
	A = a; F = f;
}

/* INC/DEC and rotate helpers */
INLINE u8 inc8(_CPUC u8 v) {
	u8 r = v + 1;
	F = (F & Z80_CF) | (r & (Z80_SF | Z80_YF | Z80_XF)) | (r ? 0 : Z80_ZF) | ((v & 0x0F) == 0x0F ? Z80_HF : 0) | (v == 0x7F ? Z80_PF : 0);
	return r;
}
INLINE u8 dec8(_CPUC u8 v) {
	u8 r = v - 1;
	F = (F & Z80_CF) | Z80_NF | (r & (Z80_SF | Z80_YF | Z80_XF)) | (r ? 0 : Z80_ZF) | ((v & 0x0F) == 0 ? Z80_HF : 0) | (v == 0x80 ? Z80_PF : 0);
	return r;
}

/* Rotate/shift: 0=RLC 1=RRC 2=RL 3=RR 4=SLA 5=SRA 6=SLL 7=SRL */
INLINE u8 rotate8(_CPUC int y, u8 val) {
	switch (y) {
	case 0: return (val << 1) | (val >> 7); case 1: return (val >> 1) | (val << 7);
	case 2: return (val << 1) | (F & Z80_CF); case 3: return (val >> 1) | ((F & Z80_CF) ? 0x80 : 0);
	case 4: return val << 1; case 5: return (val >> 1) | (val & 0x80);
	case 6: return (val << 1) | 1; case 7: return val >> 1;
	} return 0;
}

/* Default MEM_IO callbacks */
#ifdef MEM_IO
	#ifdef SINGLE_INST
		static u8 default_rd(u16 a) { return ram[a]; }
		static void default_wr(u16 a, u8 v) { ram[a] = v; }
	#else
		#undef ram
		static u8 default_rd(_CPUP, u16 a) { return cpu->ram[a]; }
		static void default_wr(_CPUC u16 a, u8 v) { cpu->ram[a] = v; }
		#define ram cpu->ram
	#endif
#endif

/* Default port I/O (IN=0xFF, OUT=no-op) */
#ifdef SINGLE_INST
	static u8 port_in_default(u16 p) { (void)p; return 0xFF; }
	static void port_out_default(u16 p, u8 v) { (void)p; (void)v; }
#else
	static u8 port_in_default(_CPUC u16 p) { (void)_CPUPA; (void)p; return 0xFF; }
	static void port_out_default(_CPUC u16 p, u8 v) { (void)_CPUPA; (void)p; (void)v; }
#endif

/* Init: set RAM, callbacks, reset */
void cpuZ80_init(_CPUC u8* ram_ptr, cpuZ80_read_cb rd_cb, cpuZ80_write_cb wr_cb, cpuZ80_in_cb pi_cb, cpuZ80_out_cb po_cb) {
	ram = ram_ptr;
	#ifdef MEM_IO
	mem_read = rd_cb ? rd_cb : default_rd; mem_write = wr_cb ? wr_cb : default_wr;
	#else
	(void)rd_cb; (void)wr_cb;
	#endif
	port_in = pi_cb ? pi_cb : port_in_default; port_out = po_cb ? po_cb : port_out_default;
	#if defined(MEM_IO) && !defined(NO_IO_MAP)
	for (int i = 0; i < 256; i++) io_map[i] = 0;
	#endif
	cpuZ80_reset(_CPUPA);
}

/* SINGLE_INST: get/set/ptr for struct access */
#ifdef SINGLE_INST
static CPUZ80* g_cpu;
void cpuZ80_set(CPUZ80* c) {
	g_cpu = c;
	rAF = c->af; rBC = c->bc; rDE = c->de; rHL = c->hl;
	rAF_ = c->af_; rBC_ = c->bc_; rDE_ = c->de_; rHL_ = c->hl_;
	rIX = c->ix; rIY = c->iy; rWZ = c->wz; rSP = c->sp; rPC = c->pc;
	I = c->I; R = c->R; IFF1 = c->IFF1; IFF2 = c->IFF2; IM = c->IM; Q = c->Q; ram = c->ram;
#ifdef MEM_IO
	mem_read = c->mem_read; mem_write = c->mem_write;
#endif
	port_in = c->port_in; port_out = c->port_out;
}
CPUZ80* cpuZ80_get(void) {
	CPUZ80* c = g_cpu;
	c->af = rAF; c->bc = rBC; c->de = rDE; c->hl = rHL;
	c->af_ = rAF_; c->bc_ = rBC_; c->de_ = rDE_; c->hl_ = rHL_;
	c->ix = rIX; c->iy = rIY; c->wz = rWZ; c->sp = rSP; c->pc = rPC;
	c->I = I; c->R = R; c->IFF1 = IFF1; c->IFF2 = IFF2; c->IM = IM; c->Q = Q; c->ram = ram;
#ifdef MEM_IO
	c->mem_read = mem_read; c->mem_write = mem_write;
#endif
	c->port_in = port_in; c->port_out = port_out; return c;
}
const CPUZ80* cpuZ80_ptr(void) { return g_cpu; }
#endif

/* IO map: mark pages as callback-handled */
#if defined(MEM_IO) && !defined(NO_IO_MAP)
	void cpuZ80_io_set(_CPUC u8 page, u8 val) { io_map[page] = val; }
	void cpuZ80_io_range(_CPUC u8 lo, u8 hi) { for (u8 p = lo; p <= hi; p++) io_map[p] = 1; }
#endif

/* Reset: AF=0xFF00, BC/DE/HL/IX/IY/SP=0xFFFF, PC=0, I=R=IFF=IM=Q=0 */
void cpuZ80_reset(_CPUP) {
	AF = 0xFF00; BC = 0xFFFF; DE = 0xFFFF; HL = 0xFFFF;
	AF_ = 0xFF00; BC_ = 0xFFFF; DE_ = 0xFFFF; HL_ = 0xFFFF;
	IX = 0xFFFF; IY = 0xFFFF; SP = 0xFFFF; PC = 0; I = 0; R = 0; IFF1 = 0; IFF2 = 0; IM = 0; Q = 0; WZ = 0;
}

/* Interrupts: IFF2=IFF1, IFF1=0, push PC, jump to vector */
void cpuZ80_irq(_CPUP) {
	if (!IFF1) return;
	IFF2 = IFF1; IFF1 = 0; INC_R(); push16(_CPUCA PC);
	if (IM == 0 || IM == 1) PC = INT_ADDR;
	else { u16 a = (u16)(I << 8) | 0xFF; PC = rd(_CPUCA a) | ((u16)rd(_CPUCA a + 1) << 8); }
}
/* NMI: IFF2=IFF1, IFF1=0, push PC, jump to $0066. */
void cpuZ80_nmi(_CPUP) {
	IFF2 = IFF1; IFF1 = 0; INC_R(); push16(_CPUCA PC); PC = NMI_ADDR;
}

/* CB prefix: rotates, bit test/set/res. BIT: Y/X from MEMPTR (HL) or register. */
static u8 cb_prefix(_CPUP) {
	Q = 0; /* CB prefix: most instructions set Q=F, but SET/RES don't modify flags */
	INC_R();
	u8 op = FB();
	INC_R();
	int y = (op >> 3) & 7, z = op & 7, x = op >> 6;
	u8 val = get_r(_CPUCA z), result;
	switch (x) {
	case 0: /* rotates/shifts */
		result = rotate8(_CPUCA y, val);
		F = ROT_CB_FLAGS(result, val, y);
		put_r(_CPUCA z, result);
		Q = F;
		return (z == 6) ? 15 : 8;
	case 1: /* BIT b,r */
		result = val & (1 << y);
		F = (F & Z80_CF) | Z80_HF | (result ? 0 : (Z80_ZF | Z80_PF)) | (result & Z80_SF);
		F |= (z == 6) ? (WZ >> 8) & (Z80_XF | Z80_YF) : val & (Z80_XF | Z80_YF);
		Q = F;
		return (z == 6) ? 12 : 8;
	case 2: /* RES b,r */
		put_r(_CPUCA z, val & ~(1 << y));
		return (z == 6) ? 15 : 8;
	case 3: /* SET b,r */
		put_r(_CPUCA z, val | (1 << y));
		return (z == 6) ? 15 : 8;
	}
	return 8;
}

/* Block instruction helpers */
/* LDI/LDD/LDIR/LDDR: dir=+1 for LDI/LDIR, -1 for LDD/LDDR. */
static u8 ld_op(_CPUC i8 dir, BOOL repeat) {
	u8 bv = rd(_CPUCA HL), sum = A + bv;
	wr(_CPUCA DE, bv);
	SET_HL(HL + dir); SET_DE(DE + dir);
	SET_BC(BC - 1);
	Q = F;
	if (repeat && BC) {
		u16 saved_pc = PC - 2;
		WZ = saved_pc + 1;
		F = (F & (Z80_SF | Z80_ZF | Z80_CF)) | Z80_PF |
			((saved_pc >> 11) & 1 ? Z80_XF : 0) | ((saved_pc >> 13) & 1 ? Z80_YF : 0);
		PC -= 2; return 21;
	}
	F = (F & (Z80_SF | Z80_ZF | Z80_CF)) | (BC ? Z80_PF : 0) |
		((sum & 0x02) ? Z80_YF : 0) | ((sum & 0x08) ? Z80_XF : 0);
	return 16;
}
/* CPI/CPD/CPIR/CPDR: dir=+1 for CPI/CPIR, -1 for CPD/CPDR. */
static u8 cp_op(_CPUC i8 dir, BOOL repeat) {
	u8 val = rd(_CPUCA HL);
	u8 r = A - val, hf = (A ^ val ^ r) & 0x10;
	u8 lo = ((A & 0x0F) - (val & 0x0F) - (hf >> 4)) & 0xFF;
	SET_HL(HL + dir); SET_BC(BC - 1);
	Q = F;
	F = Z80_NF | (F & Z80_CF) | (hf ? Z80_HF : 0) | (r & Z80_SF) | (r ? 0 : Z80_ZF) | (BC ? Z80_PF : 0);
	if (repeat && BC && r) {
		u16 saved_pc = PC - 2;
		WZ = saved_pc + 1;
		F |= ((saved_pc >> 11) & 1 ? Z80_XF : 0) | ((saved_pc >> 13) & 1 ? Z80_YF : 0);
		PC -= 2; return 21;
	}
	F |= ((lo >> 3) & 1 ? Z80_XF : 0) | ((lo >> 1) & 1 ? Z80_YF : 0);
	return 16;
}

/* Block I/O: dir=+1 for INI/OTIR, -1 for IND/OTDR. is_in=1 for IN, 0 for OUT. */
static u8 io_block(_CPUC i8 dir, BOOL repeat, BOOL is_in) {
	u16 old_BC = BC;
	u8 val = is_in ? pin(_CPUCA BC) : rd(_CPUCA HL);
	if (is_in) wr(_CPUCA HL, val); else pout(_CPUCA BC, val);
	B--;
	WZ = is_in ? old_BC + dir : BC + dir; /* IN: BC before B--; OUT: BC after B-- */
	SET_HL(HL + dir);
	Q = F;
	u16 k = is_in ? (u16)(((C + dir) & 0xFF) + val) : (u16)(L + val);
	u8 carry = k > 255;
	u8 base_P = PARITY((k & 7) ^ B);
	if (repeat && B) {
		WZ = PC - 1; /* MEMPTR = PCi+1 for repeating block I/O */
		u8 H_l = 0, P_l = base_P ^ PARITY(B & 7) ^ Z80_PF;
		if (carry) {
			if (val & 0x80) { H_l = !(B & 0x0F) ? Z80_HF : 0; P_l = base_P ^ PARITY((B - 1) & 7) ^ Z80_PF; }
			else { H_l = (B & 0x0F) == 0x0F ? Z80_HF : 0; P_l = base_P ^ PARITY((B + 1) & 7) ^ Z80_PF; }
		}
		F = (B & Z80_SF) | ((WZ >> 13) & 1 ? Z80_YF : 0) | ((WZ >> 11) & 1 ? Z80_XF : 0)
			| H_l | P_l | carry | (val & 0x80 ? Z80_NF : 0);
		PC -= 2; return 21;
	}
	F = (B & (Z80_SF | Z80_YF | Z80_XF)) | (B ? 0 : Z80_ZF)
		| (carry ? (Z80_HF | Z80_CF) : 0) | base_P | (val & 0x80 ? Z80_NF : 0);
	return 16;
}

/* ED prefix: block ops, extended instructions. Q default=0 (non-flag-modifying). */
static u8 ed_prefix(_CPUP) {
	Q = 0; /* Default: non-flag-modifying instructions set Q=0 */
	INC_R();
	u8 op = FB();
	INC_R();
	int y = (op >> 3) & 7, z = op & 7, x = op >> 6;
	u16 addr;
	u8 val;
	if (x == 0 || x == 3) return 8;
	/* Block I/O and block transfer */
	if (x == 2 && y >= 4 && z <= 3) {
		i8 dir = (y & 1) ? -1 : 1;
		BOOL rep = (y >= 6);
		switch (z) {
		case 0: return ld_op(_CPUCA dir, rep);
		case 1: return cp_op(_CPUCA dir, rep);
		case 2: return io_block(_CPUCA dir, rep, TRUE);
		case 3: return io_block(_CPUCA dir, rep, FALSE);
		}
		return 8;
	}
	/* Non-block ED instructions (y=0-3 or z=4-7) */
	switch (z) {
	case 0: /* IN r,(C) */
		val = pin(_CPUCA BC); if (y != 6) put_r(_CPUCA y & 7, val);
		F = (F & Z80_CF) | SZP_FLAGS(val); Q = F; WZ = BC + 1; return 12;
	case 1: /* OUT (C),r */
		pout(_CPUCA BC, y == 6 ? 0 : get_r(_CPUCA y & 7)); WZ = BC + 1; return 12;
	case 2: { /* SBC HL,rp / ADC HL,rp */
		u16 ci = (F & Z80_CF) ? 1 : 0, rp = get_rp(_CPUCA y >> 1, 0), old_HL = HL;
		u32 res32; u16 r16;
		if (y & 1) { /* ADC */
			res32 = (u32)HL + rp + ci; r16 = res32;
			F = (r16 ? 0 : Z80_ZF) | (r16 & Z80_S16 ? Z80_SF : 0) | ((r16 >> 8) & (Z80_YF | Z80_XF))
				| ((HL ^ rp ^ res32) & Z80_H16 ? Z80_HF : 0) | (res32 & Z80_C16 ? Z80_CF : 0)
				| (((~(HL ^ rp) & (HL ^ r16)) & Z80_S16) ? Z80_PF : 0);
		}
		else { /* SBC */
			res32 = (u32)HL - rp - ci; r16 = res32;
			F = Z80_NF | (r16 ? 0 : Z80_ZF) | (r16 & Z80_S16 ? Z80_SF : 0) | ((r16 >> 8) & (Z80_YF | Z80_XF))
				| ((HL ^ rp ^ res32) & Z80_H16 ? Z80_HF : 0) | (res32 & Z80_C16 ? Z80_CF : 0)
				| (((HL ^ rp) & (HL ^ r16) & Z80_S16) ? Z80_PF : 0);
		}
		WZ = old_HL + 1; SET_HL(r16); Q = F; return 15;
	}
	case 3: { /* LD (nn),rp / LD rp,(nn) */
		addr = F16(); if (y & 1) { u8 lo = rd(_CPUCA addr), hi = rd(_CPUCA addr + 1); set_rp(_CPUCA y >> 1, lo | ((u16)hi << 8), 0); }
		else { u16 v = get_rp(_CPUCA y >> 1, 0); wr(_CPUCA addr, v & 0xFF); wr(_CPUCA addr + 1, v >> 8); }
		WZ = addr + 1; return 20;
	}
	case 4: { /* NEG */ u16 r = 0 - A; u8 r8 = r;
		F = Z80_NF | (r8 & (Z80_SF | Z80_YF | Z80_XF)) | (r8 ? 0 : Z80_ZF) | ((0 ^ A ^ r) & Z80_HF) | (A ? Z80_CF : 0) | (A == 0x80 ? Z80_PF : 0);
		A = r8; Q = F; return 8;
	}
	case 5: /* RETN */ IFF1 = IFF2; { u16 ret = pop16(_CPUPA); WZ = ret; PC = ret; } return 14;
	case 6: /* IM */ IM = ((y & 3) == 3) ? 2 : ((y & 3) >> 1); return 8;
	case 7: switch (y) { /* misc */
	case 0: I = A; return 9;
	case 1: R = A; return 9;
	case 2: A = I; goto ld_a_ir_flags;
	case 3: A = R; ld_a_ir_flags:
		F = (F & Z80_CF) | (A & (Z80_SF | Z80_YF | Z80_XF)) | (A ? 0 : Z80_ZF) | (IFF2 ? Z80_PF : 0); Q = F; return 9;
	case 4: case 5: { /* RRD/RLD */
		val = rd(_CPUCA HL);
		if (y == 4) { wr(_CPUCA HL, (val >> 4) | (A << 4)); A = (A & 0xF0) | (val & 0x0F); }
		else { wr(_CPUCA HL, (val << 4) | (A & 0x0F)); A = (A & 0xF0) | (val >> 4); }
		F = (F & Z80_CF) | SZP_FLAGS(A); Q = F; WZ = HL + 1; return 18;
	}
	}
		break;
	}
	return 8;
}

/* DD/FD prefix: IX/IY operations. is_iy=0 for DD (IX), 1 for FD (IY). */
static u8 xy_prefix(_CPUC int is_iy, u8 prev_Q) {
	u16 xy = is_iy ? IY : IX;
	u8 op = FB();
	int y = (op >> 3) & 7, z = op & 7, x = op >> 6;
	u16 addr;
	u16 val;

	switch (op) {
	case 0xCB: /* DD CB / FD CB */
		INC_R(); addr = xy + (i8)FB(); WZ = addr; op = FB(); INC_R();
		{
			int cb_y = (op >> 3) & 7, cb_z = op & 7, cb_x = op >> 6;
			u8 v = rd(_CPUCA addr), r;
			switch (cb_x) {
			case 0: r = rotate8(_CPUCA cb_y, v); F = ROT_CB_FLAGS(r, v, cb_y);
				wr(_CPUCA addr, r); if (cb_z != 6) put_r(_CPUCA cb_z, r); Q = F; break;
			case 1: r = v & (1 << cb_y); F = (F & Z80_CF) | Z80_HF | (r ? 0 : (Z80_ZF | Z80_PF)) | (r & Z80_SF);
				F |= (WZ >> 8) & (Z80_XF | Z80_YF); Q = F; break;
			case 2: r = v & ~(1 << cb_y); wr(_CPUCA addr, r); if (cb_z != 6) put_r(_CPUCA cb_z, r); break;
			case 3: r = v | (1 << cb_y); wr(_CPUCA addr, r); if (cb_z != 6) put_r(_CPUCA cb_z, r); break;
			}
			return (cb_x == 1) ? 20 : 23;
		}
	case 0xED: return ed_prefix(_CPUPA);
	default: break;
	}
	INC_R();
	INC_R();
	switch (x) {
	case 0: /* LD/ADD/INC/DEC with IX/Iy */
		switch (z) {
		case 0: /* NOP/EX AF,DJNZ/JR - DD/FD prefix ignored, +4 cycles */
			switch (y) {
			case 0: return 8;
			case 1: EX_AFB(); Q = 0; return 8; /* EX AF,AF' clears Q */
			case 2: { i8 d = (i8)FB(); B--; if (B) { PC += d; return 17; } return 12; }
			case 3: { i8 d = (i8)FB(); PC += d; WZ = PC; return 16; }
			default: { i8 d = (i8)FB(); if (cc(_CPUCA y - 4)) { PC += d; WZ = PC; return 16; } return 11; }
			}
		case 1:
			if (y & 1) { /* ADD IX/IY,rp */
				u16 v = (y >> 1) == 2 ? xy : get_rp(_CPUCA y >> 1, 0); u32 r = xy + v;
				F = (F & (Z80_SF | Z80_ZF | Z80_PF)) | ((xy ^ v ^ r) & Z80_H16 ? Z80_HF : 0) | (r & Z80_C16 ? Z80_CF : 0) | ((r >> 8) & (Z80_YF | Z80_XF));
				WZ = xy + 1; SET_XY(r & 0xFFFF); Q = F;
			}
			else { /* LD rp,nn */
				val = F16(); if ((y >> 1) == 2) SET_XY(val); else set_rp(_CPUCA y >> 1, val, 0);
			}
			return (y & 1) ? 15 : 14;
		case 2: switch (y) {
		case 0: wr(_CPUCA BC, A); WZ = (A << 8) | (BC & 0xFF); return 11;
		case 1: A = rd(_CPUCA BC); WZ = (A << 8) | ((BC + 1) & 0xFF); return 11;
		case 2: wr(_CPUCA DE, A); WZ = (A << 8) | (DE & 0xFF); return 11;
		case 3: A = rd(_CPUCA DE); WZ = (A << 8) | ((DE + 1) & 0xFF); return 11;
		case 4: addr = F16(); wr(_CPUCA addr, xy & 0xFF); wr(_CPUCA addr + 1, xy >> 8); WZ = addr + 1; return 20;
		case 5: addr = F16(); SET_XY(rd(_CPUCA addr) | ((u16)rd(_CPUCA addr + 1) << 8)); WZ = addr + 1; return 20;
		case 6: addr = F16(); wr(_CPUCA addr, A); WZ = (A << 8) | (addr & 0xFF); return 17;
		case 7: addr = F16(); A = rd(_CPUCA addr); WZ = (A << 8) | ((addr + 1) & 0xFF); return 17;
		} break;
		case 3: /* INC/DEC rp */
			if (y & 1) { if ((y >> 1) == 2) SET_XY(xy - 1); else set_rp(_CPUCA y >> 1, get_rp(_CPUCA y >> 1, 0) - 1, 0); }
			else { if ((y >> 1) == 2) SET_XY(xy + 1); else set_rp(_CPUCA y >> 1, get_rp(_CPUCA y >> 1, 0) + 1, 0); }
			return 10;
		case 4: if (y == 6) { addr = xy + (i8)FB(); WZ = addr; wr(_CPUCA addr, inc8(_CPUCA rd(_CPUCA addr))); Q = F; return 23; } else { put_r_ix(_CPUCA y, inc8(_CPUCA get_r_ix(_CPUCA y, xy)), is_iy); Q = F; return 8; }
		case 5: if (y == 6) { addr = xy + (i8)FB(); WZ = addr; wr(_CPUCA addr, dec8(_CPUCA rd(_CPUCA addr))); Q = F; return 23; } else { put_r_ix(_CPUCA y, dec8(_CPUCA get_r_ix(_CPUCA y, xy)), is_iy); Q = F; return 8; }
		case 6: if (y == 6) { addr = xy + (i8)FB(); WZ = addr; val = FB(); wr(_CPUCA addr, val); Q = 0; return 19; } else { val = FB(); put_r_ix(_CPUCA y, val, is_iy); Q = 0; return 11; }
		case 7: switch (y) {
		case 0: case 1: case 2: case 3: ROT_A_INST(y); return 8;
		case 4: DAA_INST(); return 8;
		case 5: CPL_INST(); return 8;
		case 6: SCF_INST(prev_Q); return 8;
		case 7: CCF_INST(prev_Q); return 8;
		} break;
		}
		break;
	case 1: /* LD r,r' IX/IY displacement */
		if (op == 0x76) return 8; /* HALT */
		if (y == 6 && z == 6) return 8; /* NOP */
		if (z == 6) { addr = xy + (i8)FB(); WZ = addr; if (y != 6) put_r(_CPUCA y, rd(_CPUCA addr)); Q = 0; return 19; }
		else if (y == 6) { addr = xy + (i8)FB(); WZ = addr; wr(_CPUCA addr, get_r(_CPUCA z)); return 19; }
		else { put_r_ix(_CPUCA y, get_r_ix(_CPUCA z, xy), is_iy); Q = 0; return 8; }
	case 2: /* ALU with IX/IY displacement */
		if (z == 6) { addr = xy + (i8)FB(); WZ = addr; alu(_CPUCA y, rd(_CPUCA addr)); Q = F; return 19; }
		else { alu(_CPUCA y, get_r_ix(_CPUCA z, xy)); Q = F; return 8; }
	case 3: /* jumps/calls/push/pop with IX/IY */
		switch (z) {
		case 0: if (cc(_CPUCA y)) { u16 ret = pop16(_CPUPA); PC = ret; WZ = ret; Q = 0; return 15; } Q = 0; return 9;
		case 1: if (y & 1) {
			switch (y >> 1) {
			case 0: { u16 ret = pop16(_CPUPA); PC = ret; WZ = ret; Q = 0; return 14; }
			case 1: EXX(); return 8;
			case 2: PC = xy; Q = 0; return 8;
			case 3: SP = xy; return 10;
			}
		}
			else {
			if ((y >> 1) == 2) { u16 v = pop16(_CPUPA); SET_XY(v); }
			else set_rp(_CPUCA y >> 1, pop16(_CPUPA), 1);
			Q = 0; return 14;
		} break;
		case 2: addr = F16(); WZ = addr; if (cc(_CPUCA y)) PC = addr; Q = 0; return 14;
		case 3: if (y == 4) { /* EX (SP),IX/IY */
			u8 lo = rd(_CPUCA SP), hi = rd(_CPUCA SP + 1); wr(_CPUCA SP, xy & 0xFF); wr(_CPUCA SP + 1, xy >> 8);
			SET_XY(lo | ((u16)hi << 8)); WZ = lo | ((u16)hi << 8); return 23;
		}
			else break;
		case 4: addr = F16(); WZ = addr; if (cc(_CPUCA y)) { push16(_CPUCA PC); PC = addr; return 21; }
			else return 14;
		case 5: if (y & 1) { addr = F16(); WZ = addr; push16(_CPUCA PC); PC = addr; return 21; }
			else { if ((y >> 1) == 2) push16(_CPUCA xy); else push16(_CPUCA get_rp(_CPUCA y >> 1, 1)); return 15; }
		default: break;
		}
		break;
	}
	/* DD/FD prefix: unchanged instructions (+4 cycles) */
	if (x == 3) {
		if (z == 3) {
			switch (y) {
			case 0: addr = F16(); WZ = addr; PC = addr; Q = 0; return 14;
			case 2: { u8 p = FB(); pout(_CPUCA((u16)A << 8) | p, A); WZ = ((u16)A << 8) | ((p + 1) & 0xFF); Q = F; return 15; }
			case 3: { u8 p = FB(); u8 old_A = A; A = pin(_CPUCA((u16)old_A << 8) | p); WZ = (((u16)old_A << 8) | p) + 1; Q = 0; return 15; }
			case 5: { u16 t = DE; SET_DE(HL); SET_HL(t); return 8; }
			case 6: IFF1 = 0; IFF2 = 0; Q = 0; return 8;
			case 7: IFF1 = 1; IFF2 = 1; Q = 0; return 8;
			}
		}
		else if (z == 6) { alu(_CPUCA y, FB()); Q = F; return 11; }
		else if (z == 7) { push16(_CPUCA PC); PC = y * 8; return 15; }
	}
	return 8;
}

/* Execute non-prefix opcode (x/y/z decoded dispatch). INLINE so the compiler can optimize this hot body independently within cpuZ80_step. */
INLINE u8 step_exec(_CPUC u8 op, u8 old_Q)
{
	STEPSTART;
	u16 addr;
	u8 val;
	(void)addr; (void)val;
	int y = (op >> 3) & 7, z = op & 7, x = op >> 6;
	int p = y >> 1, q = y & 1;
	INC_R();

	switch (x) {
	case 0: /* misc: NOP/DJNZ/JR/LD/ADD/INC/DEC/rotates/DAA/CPL/SCF/CCF */
		switch (z) {
		case 0: switch (y) {
		case 0: CYC(4); break;
		case 1: EX_AFB(); Q = 0; CYC(4); break;
		case 2: { i8 d = (i8)FB(); B--; if (B) { PC += d; WZ = PC; CYC(13); } else { CYC(8); } break; }
		case 3: { i8 d = (i8)FB(); PC += d; WZ = PC; CYC(12); break; }
		default: { i8 d = (i8)FB(); if (cc(_CPUCA y - 4)) { PC += d; WZ = PC; CYC(12); } else { CYC(7); } break; }
		} break;
		case 1: if (q) { /* ADD HL,rp */
			u16 v = (p == 2) ? HL : get_rp(_CPUCA p, 0); u32 r = HL + v;
			F = (F & (Z80_SF | Z80_ZF | Z80_PF)) | ((HL ^ v ^ r) & Z80_H16 ? Z80_HF : 0) | (r & Z80_C16 ? Z80_CF : 0) | ((r >> 8) & (Z80_YF | Z80_XF));
			WZ = HL + 1; SET_HL(r & 0xFFFF); Q = F; CYC(11);
		}
			else { set_rp(_CPUCA p, F16(), 0); CYC(10); } break;
		case 2: switch (y) {
		case 0: wr(_CPUCA BC, A); WZ = (A << 8) | ((BC + 1) & 0xFF); CYC(7); break;
		case 1: A = rd(_CPUCA BC); WZ = BC + 1; CYC(7); break;
		case 2: wr(_CPUCA DE, A); WZ = (A << 8) | ((DE + 1) & 0xFF); CYC(7); break;
		case 3: A = rd(_CPUCA DE); WZ = DE + 1; CYC(7); break;
		case 4: addr = F16(); wr(_CPUCA addr, L); wr(_CPUCA addr + 1, H); WZ = addr + 1; CYC(16); break;
		case 5: addr = F16(); L = rd(_CPUCA addr); H = rd(_CPUCA addr + 1); WZ = addr + 1; CYC(16); break;
		case 6: addr = F16(); wr(_CPUCA addr, A); WZ = (A << 8) | ((addr + 1) & 0xFF); CYC(13); break;
		case 7: addr = F16(); A = rd(_CPUCA addr); WZ = addr + 1; CYC(13); break;
		} break;
		case 3: if (q) set_rp(_CPUCA p, get_rp(_CPUCA p, 0) - 1, 0); else set_rp(_CPUCA p, get_rp(_CPUCA p, 0) + 1, 0); CYC(6); break;
		case 4: put_r(_CPUCA y, inc8(_CPUCA get_r(_CPUCA y))); Q = F; CYC(y == 6 ? 11 : 4); break;
		case 5: put_r(_CPUCA y, dec8(_CPUCA get_r(_CPUCA y))); Q = F; CYC(y == 6 ? 11 : 4); break;
		case 6: if (y == 6) { wr(_CPUCA HL, FB()); CYC(10); } else { put_r(_CPUCA y, FB()); CYC(7); } break;
		case 7: switch (y) {
		case 0: case 1: case 2: case 3: ROT_A_INST(y); CYC(4); break;
		case 4: DAA_INST(); CYC(4); break;
		case 5: CPL_INST(); CYC(4); break;
		case 6: SCF_INST(old_Q); CYC(4); break;
		case 7: CCF_INST(old_Q); CYC(4); break;
		} break;
		}
		break;
	case 1: /* LD r,r' (including HALT) */
		if (op == 0x76) { CYC(4); break; }
		put_r(_CPUCA y, get_r(_CPUCA z)); Q = 0; CYC((y == 6 || z == 6) ? 7 : 4); break;
	case 2: /* ALU A,r */
		alu(_CPUCA y, get_r(_CPUCA z)); Q = F; CYC(z == 6 ? 7 : 4); break;
	case 3: /* jumps, calls, returns, misc */
		switch (z) {
		case 0: if (cc(_CPUCA y)) { u16 ret = pop16(_CPUPA); PC = ret; WZ = ret; CYC(11); }
			else { CYC(5); }
			Q = 0;
			break;
		case 1:
			if (q) {
				switch (p) {
				case 0: { u16 ret = pop16(_CPUPA); PC = ret; WZ = ret; Q = 0; CYC(10); break; }
				case 1: EXX(); CYC(4); break;
				case 2: PC = HL; Q = 0; CYC(4); break;
				case 3: SP = HL; CYC(6); break;
				}
			}
			else { set_rp(_CPUCA p, pop16(_CPUPA), 1); Q = 0; CYC(10); }
			break;
		case 2: addr = F16(); WZ = addr; if (cc(_CPUCA y)) PC = addr; Q = 0; CYC(10); break;
		case 3: switch (y) {
		case 0: addr = F16(); WZ = addr; PC = addr; Q = 0; CYC(10); break;
		case 1: { u8 r = cb_prefix(_CPUPA); CYC(r); break; }
		case 2: { u8 p = FB(); pout(_CPUCA((u16)A << 8) | p, A); WZ = ((u16)A << 8) | ((p + 1) & 0xFF); CYC(11); break; }
		case 3: { u8 p = FB(); u8 old_A = A; A = pin(_CPUCA((u16)old_A << 8) | p); WZ = (((u16)old_A << 8) | p) + 1; CYC(11); break; }
		case 4: { u8 lo = rd(_CPUCA SP), hi = rd(_CPUCA SP + 1); wr(_CPUCA SP, L); wr(_CPUCA SP + 1, H); H = hi; L = lo; WZ = ((u16)hi << 8) | lo; CYC(19); break; }
		case 5: { u16 t = DE; SET_DE(HL); SET_HL(t); CYC(4); break; }
		case 6: IFF1 = IFF2 = 0; Q = 0; CYC(4); break;
		case 7: IFF1 = IFF2 = 1; Q = 0; CYC(4); break;
		} break;
		case 4: addr = F16(); WZ = addr; if (cc(_CPUCA y)) { push16(_CPUCA PC); PC = addr; CYC(17); } else { CYC(10); } break;
		case 5: if (q) { addr = F16(); WZ = addr; push16(_CPUCA PC); PC = addr; CYC(17); } else { push16(_CPUCA get_rp(_CPUCA p, 1)); CYC(11); } break;
		case 6: alu(_CPUCA y, FB()); Q = F; CYC(7); break;
		case 7: push16(_CPUCA PC); PC = y * 8; CYC(11); break;
		}
		break;
	}
	return STEPRET;
}

/* Main step: fetch opcode, dispatch to prefix handlers or execute via inline step_exec.
 * Uses direct switch for prefix bytes (best branch prediction for single-step),
 * inline step_exec for non-prefix opcodes (avoids indirect call overhead). */
CYCLES cpuZ80_step(_CPUP)
{
	STEPSTART;
	u8 old_Q = Q; Q = 0;
	u8 op = FB();
	switch (op) {
	case 0xCB: { u8 r = cb_prefix(_CPUPA); CYC(r); break; }
	case 0xDD: { u8 r = xy_prefix(_CPUCA 0, old_Q); CYC(r); break; }
	case 0xED: { u8 r = ed_prefix(_CPUPA); CYC(r); break; }
	case 0xFD: { u8 r = xy_prefix(_CPUCA 1, old_Q); CYC(r); break; }
	default: { u8 r = step_exec(_CPUCA op, old_Q); CYC(r); break; }
	}
	return STEPRET;
}

#ifdef COUNT_CYCLES
/* Batch: run until max_cycles consumed, return cycles used */
u32 cpuZ80_run(_CPUC u32 budget) {
	u32 cycles = 0;
	while (cycles < budget) {
		cycles += cpuZ80_step(_CPUPA);
	}
	return cycles;
}
#endif

#ifdef DEBUG
void cpuZ80_dump(_CCPUP) {
	printf("AF =%04X BC =%04X DE =%04X HL =%04X IX=%04X IY=%04X SP=%04X PC=%04X\n", AF, BC, DE, HL, IX, IY, SP, PC);
	printf("AF'=%04X BC'=%04X DE'=%04X HL'=%04X I=%02X R=%02X IM=%d IFF1=%d IFF2=%d\n", AF_, BC_, DE_, HL_, I, R, IM, IFF1, IFF2);
}
#endif
