#ifdef DEBUG
	#include <stdio.h>
#endif
#include <string.h>
#include "cpu65816.h"

/* Emulation-mode vectors (bank 0) */
#define NMI_VEC_E	0xFFFA	/* NMI vector: $FFFA/$FFFB */
#define RST_VEC_E	0xFFFC	/* Reset vector: $FFFC/$FFFD */
#define IRQ_VEC_E	0xFFFE	/* IRQ/BRK vector: $FFFE/$FFFF */

/* Native-mode vectors (bank 0) */
#define COP_VEC_E	0xFFF4	/* COP vector (emu): $FFF4/$FFF5 */
#define COP_VEC_N	0xFFE4	/* COP vector: $FFE4/$FFE5 */
#define BRK_VEC_N	0xFFE6	/* BRK vector: $FFE6/$FFE7 */
#define ABT_VEC_N	0xFFE8	/* ABT vector: $FFE8/$FFE9 */
#define NMI_VEC_N	0xFFEA	/* NMI vector: $FFEA/$FFEB */
#define IRQ_VEC_N	0xFFEE	/* IRQ vector: $FFEE/$FFEF */

/* Stack: emu mode forces page 1 ($0100-$01FF), native uses full 16-bit SP. */
#define STACK_PAGE_E	0x0100	/* stack page in emulation mode */
#define INIT_SP_E	0xFF	/* SP low byte after reset (emu: $01FF) */
#define INIT_SP_N	0x01FF	/* SP after reset (native) */
#define ADDR_MASK	0xFFFF	/* 16-bit address wrap */

/* SINGLE_INST: bare static globals, no cpu pointer param */
#ifdef SINGLE_INST
	static uint16_t A, X, Y, SP, PC, D;
	static uint8_t P, PBR, DBR, E;
	static uint8_t* ram;
	static BOOL halted;
	static void* ctx;
	#ifdef COUNT_CYCLES
		static uint8_t waiting;
	#endif
	#ifdef MEM_IO
		static cpu65816_read_cb mem_read;
		static cpu65816_write_cb mem_write;
	#endif
#else
	#define A cpu->A
	#define X cpu->X
	#define Y cpu->Y
	#define SP cpu->SP
	#define PC cpu->PC
	#define D cpu->D
	#define P cpu->P
	#define PBR cpu->PBR
	#define DBR cpu->DBR
	#define E cpu->E
	#define ram cpu->ram
	#define halted cpu->halted
	#define ctx cpu->ctx
	#ifdef COUNT_CYCLES
		#define waiting cpu->waiting
	#endif
	#ifdef MEM_IO
		#define mem_read cpu->mem_read
		#define mem_write cpu->mem_write
	#endif
#endif

/* I/O dispatch */
#if !defined(MEM_IO)
	#define IO_RD(a) ram[(a)]
	#define IO_WR(a,v) ram[(a)] = (v)
#elif defined(NO_IO_MAP)
	#define IO_RD(a) mem_read(_CPUCA (uint32_t)(a))
	#define IO_WR(a,v) mem_write(_CPUCA (uint32_t)(a), (v))
#elif defined(SINGLE_INST)
	#define IO_RD(a) (io_map[(a)>>8] ? mem_read((uint32_t)(a)) : ram[(a)])
	#define IO_WR(a,v) do { if (io_map[(a)>>8]) mem_write((uint32_t)(a), (v)); else ram[(a)] = (v); } while(0)
#else
	#define IO_RD(a) (io_map[(a)>>8] ? mem_read(cpu, (uint32_t)(a)) : ram[(a)])
	#define IO_WR(a,v) do { if (io_map[(a)>>8]) mem_write(cpu, (uint32_t)(a), (v)); else ram[(a)] = (v); } while(0)
#endif

#if defined(MEM_IO) && !defined(NO_IO_MAP)
	static uint8_t io_map[65536];
#endif

/* Memory helpers */
INLINE uint8_t rd8(_CPUC uint32_t a) { return IO_RD(a); }
INLINE uint16_t rd16(_CPUC uint32_t a) { return (uint16_t)IO_RD(a) | ((uint16_t)IO_RD(a + 1) << 8); }
INLINE uint16_t rd16_wrap(_CPUC uint32_t a) { return (uint16_t)IO_RD(a) | ((uint16_t)IO_RD((a & 0xFFFF0000) | ((a + 1) & 0xFFFF)) << 8); }
INLINE void wr8(_CPUC uint32_t a, uint8_t v) { IO_WR(a, v); }
INLINE void wr16(_CPUC uint32_t a, uint16_t v) { IO_WR(a, v & 0xFF); IO_WR(a + 1, (v >> 8) & 0xFF); }
INLINE void wr16_wrap(_CPUC uint32_t a, uint16_t v) { IO_WR(a, v & 0xFF); IO_WR((a & 0xFFFF0000) | ((a + 1) & 0xFFFF), (v >> 8) & 0xFF); }

#ifdef SINGLE_INST
	#define rd8(c,a) rd8(a)
	#define rd16(c,a) rd16(a)
	#define rd16_wrap(c,a) rd16_wrap(a)
	#define wr8(c,a,v) wr8(a,v)
	#define wr16(c,a,v) wr16(a,v)
	#define wr16_wrap(c,a,v) wr16_wrap(a,v)
#endif

/* M/X flag helpers */
#define M8 (P & P65816_M)
#define X8 (P & P65816_X)
#define DP_OFF (uint8_t)(D & 0xFF)

/* Flag setting */
#define SETNZ8(v) do { uint8_t _v = (v); P = (P & ~(P65816_N|P65816_Z)) | (_v & P65816_N) | (_v ? 0 : P65816_Z); } while(0)
#define SETNZ16(v) do { uint16_t _v = (v); P = (P & ~(P65816_N|P65816_Z)) | ((_v >> 8) & P65816_N) | (_v ? 0 : P65816_Z); } while(0)
#define SETNZ_A(v) do { if (M8) SETNZ8(v); else SETNZ16(v); } while(0)
#define SETNZ_XY(v) do { if (X8) SETNZ8(v); else SETNZ16(v); } while(0)
#define SETC(v) do { P = (P & ~P65816_C) | ((v) ? P65816_C : 0); } while(0)

/* Stack: emu mode SP high=$01, low byte wraps 8-bit. Native: full 16-bit SP. */
#define PUSH8(v) do { \
	if (E) { wr8(_CPUCA STACK_PAGE_E | (SP & 0xFF), (v)); SP = STACK_PAGE_E | ((SP - 1) & 0xFF); } \
	else { wr8(_CPUCA SP, (v)); SP--; } \
} while(0)
#define PULL8() (E ? (SP = STACK_PAGE_E | ((SP + 1) & 0xFF), rd8(_CPUCA SP)) : (SP++, rd8(_CPUCA SP)))
#define PUSH16(v) do { PUSH8((v) >> 8); PUSH8((v) & 0xFF); } while(0)
/* PULL16 uses _pull16_tmp declared in step() — nested comma operators force left-to-right evaluation */
#define PULL16() (_pull16_tmp = PULL8(), _pull16_tmp |= ((uint16_t)PULL8() << 8), _pull16_tmp)

/* Addressing mode inline functions.
 * _W suffix: DP/SR modes that wrap 16-bit within bank 0.
 * Others: absolute/long modes that can cross banks. */
INLINE uint32_t dp_a(_CPUC uint8_t off) { return (uint32_t)((D + (uint8_t)(off)) & 0xFFFF); }
INLINE uint32_t dpx(_CPUC uint8_t off) {
	uint16_t idx = X8 ? (X & 0xFF) : X;
	uint16_t base = (D & 0xFF) == 0 && E ? (uint8_t)((uint8_t)(off) + (idx & 0xFF)) : (uint16_t)((uint8_t)(off) + idx);
	return (uint32_t)((D + base) & 0xFFFF);
}
INLINE uint32_t dpy(_CPUC uint8_t off) {
	uint16_t idx = X8 ? (Y & 0xFF) : Y;
	uint16_t base = (D & 0xFF) == 0 && E ? (uint8_t)((uint8_t)(off) + (idx & 0xFF)) : (uint16_t)((uint8_t)(off) + idx);
	return (uint32_t)((D + base) & 0xFFFF);
}
INLINE uint32_t abs_a(_CPUC uint16_t addr) { return ((uint32_t)DBR << 16) | addr; }
INLINE uint32_t absx(_CPUC uint16_t addr) {
	uint32_t base = (uint32_t)DBR << 16;
	return (base + (uint16_t)addr + (X8 ? (X & 0xFF) : X)) & 0xFFFFFF;
}
INLINE uint32_t absy(_CPUC uint16_t addr) {
	uint32_t base = (uint32_t)DBR << 16;
	return (base + (uint16_t)addr + (X8 ? (Y & 0xFF) : Y)) & 0xFFFFFF;
}
INLINE uint32_t absxl(_CPUC uint32_t addr) {
	return (uint32_t)((addr & 0xFFFFFF) + (X8 ? (X & 0xFF) : X)) & 0xFFFFFF;
}
INLINE uint32_t indx(_CPUC uint8_t off) {
	uint16_t idx = X8 ? (X & 0xFF) : X;
	uint16_t base = (D & 0xFF) == 0 && E ? (uint8_t)(off + idx) : (uint16_t)(off + idx);
	uint32_t a = (D + base) & 0xFFFF;
	return ((uint32_t)DBR << 16) | rd16_wrap(_CPUCA a);
}
INLINE uint32_t indy(_CPUC uint8_t off) {
	uint32_t a = dp_a(_CPUCA off);
	uint16_t ptr = rd16_wrap(_CPUCA a);
	uint32_t base = (uint32_t)DBR << 16;
	return (base + ptr + (X8 ? (Y & 0xFF) : Y)) & 0xFFFFFF;
}
INLINE uint32_t indp(_CPUC uint8_t off) {
	uint32_t a = dp_a(_CPUCA off);
	return ((uint32_t)DBR << 16) | rd16_wrap(_CPUCA a);
}
INLINE uint32_t indpl(_CPUC uint8_t off) {
	uint32_t a = dp_a(_CPUCA off);
	return rd8(_CPUCA a) | ((uint16_t)rd8(_CPUCA (a & 0xFFFF0000) | ((a + 1) & 0xFFFF)) << 8)
		| ((uint32_t)rd8(_CPUCA (a & 0xFFFF0000) | ((a + 2) & 0xFFFF)) << 16);
}
INLINE uint32_t indly(_CPUC uint8_t off) {
	uint32_t a = dp_a(_CPUCA off);
	uint32_t ptr = rd8(_CPUCA a) | ((uint16_t)rd8(_CPUCA (a & 0xFFFF0000) | ((a + 1) & 0xFFFF)) << 8)
		| ((uint32_t)rd8(_CPUCA (a & 0xFFFF0000) | ((a + 2) & 0xFFFF)) << 16);
	uint16_t y = X8 ? (Y & 0xFF) : Y;
	return (uint32_t)((ptr + y) & 0xFFFFFF);
}
INLINE uint32_t sr(_CPUC uint8_t off) { return (uint32_t)((SP + off) & 0xFFFF); }
INLINE uint32_t sriy(_CPUC uint8_t off) {
	uint32_t a = (uint32_t)((SP + off) & 0xFFFF);
	uint16_t ptr = rd16_wrap(_CPUCA a);
	uint32_t base = (uint32_t)DBR << 16;
	return (base + ptr + (X8 ? (Y & 0xFF) : Y)) & 0xFFFFFF;
}
INLINE uint32_t ind(_CPUC uint16_t addr) {
	uint32_t bank = (uint32_t)PBR << 16;
	return ((uint32_t)PBR << 16) | rd16_wrap(_CPUCA bank | addr);
}
INLINE uint32_t indhl(_CPUC uint16_t addr) {
	uint32_t a = ((uint32_t)PBR << 16) | addr;
	uint32_t b = a & 0xFFFF0000;
	return rd8(_CPUCA a) | ((uint16_t)rd8(_CPUCA b | (((uint16_t)a + 1) & 0xFFFF)) << 8)
		| ((uint32_t)rd8(_CPUCA b | (((uint16_t)a + 2) & 0xFFFF)) << 16);
}
INLINE uint32_t inda(_CPUC uint16_t addr) {
	uint32_t a = ((uint32_t)PBR << 16) | (uint16_t)(addr + (X8 ? (X & 0xFF) : X));
	return ((uint32_t)PBR << 16) | rd16_wrap(_CPUCA a);
}

#ifdef SINGLE_INST
	#define dp_a(c,o) dp_a(o)
	#define dpx(c,o) dpx(o)
	#define dpy(c,o) dpy(o)
	#define abs_a(c,a) abs_a(a)
	#define absx(c,a) absx(a)
	#define absy(c,a) absy(a)
	#define absxl(c,a) absxl(a)
	#define indx(c,o) indx(o)
	#define indy(c,o) indy(o)
	#define indp(c,o) indp(o)
	#define indpl(c,o) indpl(o)
	#define indly(c,o) indly(o)
	#define sr(c,o) sr(o)
	#define sriy(c,o) sriy(o)
	#define ind(c,a) ind(a)
	#define indhl(c,a) indhl(a)
	#define inda(c,a) inda(a)
#endif

/* Fetch macros (used inside step() scope — _fb is a local temp) */
#define F() (_fb = IO_RD(((uint32_t)PBR << 16) | PC), PC++, _fb)
#define F16() (_fb16 = rd16_wrap(_CPUCA ((uint32_t)PBR << 16) | PC), PC += 2, _fb16)
#define F24() do { uint32_t _fa = ((uint32_t)PBR << 16) | PC; \
	ea = (uint32_t)IO_RD(_fa) | ((uint32_t)IO_RD((_fa & 0xFFFF0000) | ((_fa + 1) & 0xFFFF)) << 8) \
	| ((uint32_t)IO_RD((_fa & 0xFFFF0000) | ((_fa + 2) & 0xFFFF)) << 16); PC += 3; } while(0)

/* ALU operation macros: _W variants use rd16_wrap/wr16_wrap for DP/SR bank wrapping */
#define _LOGICAL_EA(ea_val, op, rd16_fn) do { \
	if (M8) { A = (A & 0xFF00) | ((A & 0xFF) op rd8(_CPUCA (ea_val))); SETNZ8(A & 0xFF); } \
	else { A op##= rd16_fn(_CPUCA (ea_val)); SETNZ16(A); } \
} while(0)

#define ORA_EA(ea_val) _LOGICAL_EA(ea_val, |, rd16)
#define ORA_EA_W(ea_val) _LOGICAL_EA(ea_val, |, rd16_wrap)
#define AND_EA(ea_val) _LOGICAL_EA(ea_val, &, rd16)
#define AND_EA_W(ea_val) _LOGICAL_EA(ea_val, &, rd16_wrap)
#define EOR_EA(ea_val) _LOGICAL_EA(ea_val, ^, rd16)
#define EOR_EA_W(ea_val) _LOGICAL_EA(ea_val, ^, rd16_wrap)

#define LDA_EA(ea_val) do { if (M8) { A = (A & 0xFF00) | rd8(_CPUCA (ea_val)); SETNZ8(A & 0xFF); } else { A = rd16(_CPUCA (ea_val)); SETNZ16(A); } } while(0)
#define LDA_EA_W(ea_val) do { if (M8) { A = (A & 0xFF00) | rd8(_CPUCA (ea_val)); SETNZ8(A & 0xFF); } else { A = rd16_wrap(_CPUCA (ea_val)); SETNZ16(A); } } while(0)
#define STA_EA(ea_val) do { if (M8) wr8(_CPUCA (ea_val), A & 0xFF); else wr16(_CPUCA (ea_val), A); } while(0)
#define STA_EA_W(ea_val) do { if (M8) wr8(_CPUCA (ea_val), A & 0xFF); else wr16_wrap(_CPUCA (ea_val), A); } while(0)

#define CMP_EA(ea_val) do { \
	if (M8) { uint8_t _v = rd8(_CPUCA (ea_val)); uint8_t _c = (A & 0xFF) - _v; SETC((A & 0xFF) >= _v); SETNZ8(_c); } \
	else { uint16_t _v = rd16(_CPUCA (ea_val)); uint16_t _c = A - _v; SETC(A >= _v); SETNZ16(_c); } \
} while(0)
#define CMP_EA_W(ea_val) do { \
	if (M8) { uint8_t _v = rd8(_CPUCA (ea_val)); uint8_t _c = (A & 0xFF) - _v; SETC((A & 0xFF) >= _v); SETNZ8(_c); } \
	else { uint16_t _v = rd16_wrap(_CPUCA (ea_val)); uint16_t _c = A - _v; SETC(A >= _v); SETNZ16(_c); } \
} while(0)

/* CPX/CPY: compare index register — immediate and memory modes */
#define CMP_XY_IMM(reg) do { \
	if (X8) { uint8_t _v = F(); uint8_t _c = (reg & 0xFF) - _v; SETC((reg & 0xFF) >= _v); SETNZ8(_c); CYC(2); } \
	else { uint16_t _v = F16(); uint16_t _c = reg - _v; SETC(reg >= _v); SETNZ16(_c); CYC(3); } \
} while(0)
#define CMP_XY_MEM(reg, ea_val, rd16_fn) do { \
	if (X8) { uint8_t _v = rd8(_CPUCA (ea_val)); uint8_t _c = (reg & 0xFF) - _v; SETC((reg & 0xFF) >= _v); SETNZ8(_c); } \
	else { uint16_t _v = rd16_fn(_CPUCA (ea_val)); uint16_t _c = reg - _v; SETC(reg >= _v); SETNZ16(_c); } \
} while(0)

/* BIT memory — sets N/V/Z flags from memory value (immediate BIT only sets Z) */
#define BIT_EA(ea_val, rd16_fn) do { \
	if (M8) { uint8_t v = rd8(_CPUCA (ea_val)); \
		P = (P & ~(P65816_N|P65816_V|P65816_Z)) | (v & (P65816_N|P65816_V)) | ((v & (A & 0xFF)) ? 0 : P65816_Z); } \
	else { uint16_t v = rd16_fn(_CPUCA (ea_val)); \
		P = (P & ~(P65816_N|P65816_V|P65816_Z)) | ((v & 0x8000) ? P65816_N : 0) | ((v & 0x4000) ? P65816_V : 0) | ((v & A) ? 0 : P65816_Z); } \
} while(0)

/* Immediate-mode ALU macros — each fetches F()/F16() and applies the operation */
#define ORA_IMM do { if (M8) { uint8_t _v = F(); A = (A & 0xFF00) | ((A & 0xFF) | _v); SETNZ8(A & 0xFF); CYC(2); } else { uint16_t _v = F16(); A |= _v; SETNZ16(A); CYC(3); } } while(0)
#define AND_IMM do { if (M8) { uint8_t _v = F(); A = (A & 0xFF00) | ((A & 0xFF) & _v); SETNZ8(A & 0xFF); CYC(2); } else { uint16_t _v = F16(); A &= _v; SETNZ16(A); CYC(3); } } while(0)
#define EOR_IMM do { if (M8) { uint8_t _v = F(); A = (A & 0xFF00) | ((A & 0xFF) ^ _v); SETNZ8(A & 0xFF); CYC(2); } else { uint16_t _v = F16(); A ^= _v; SETNZ16(A); CYC(3); } } while(0)
#define ADC_IMM do { if (M8) { uint8_t _v = F(); DO_ADC_8(A & 0xFF, _v); CYC(2); } else { uint16_t _v = F16(); DO_ADC_16(_v); CYC(3); } } while(0)
#define SBC_IMM do { if (M8) { uint8_t _v = F(); DO_SBC_8(A & 0xFF, _v); CYC(2); } else { uint16_t _v = F16(); DO_SBC_16(_v); CYC(3); } } while(0)
#define CMP_IMM do { if (M8) { uint8_t _v = F(); uint8_t _c = (A & 0xFF) - _v; SETC((A & 0xFF) >= _v); SETNZ8(_c); CYC(2); } else { uint16_t _v = F16(); uint16_t _c = A - _v; SETC(A >= _v); SETNZ16(_c); CYC(3); } } while(0)
#define LDA_IMM do { if (M8) { A = (A & 0xFF00) | F(); SETNZ8(A & 0xFF); CYC(2); } else { A = F16(); SETNZ16(A); CYC(3); } } while(0)

/* 15 addressing modes for read-ALU operations (ORA/AND/EOR/CMP/LDA/ADC/SBC)
 * op_imm: name of an _IMM macro that handles the immediate case (fetches F()/F16() itself) */
#define ALU_MODES(base, op_ea, op_ea_w, op_imm) \
	case (base|0x01): { uint8_t _dp = F(); ea = indx(_CPUCA _dp); op_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x05): { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); op_ea_w(ea); CYC(3); if (DP_OFF) CYC(1); break; } \
	case (base|0x09): { op_imm; break; } \
	case (base|0x0D): { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); op_ea(ea); CYC(4); break; } \
	case (base|0x11): { uint8_t _dp = F(); ea = indy(_CPUCA _dp); op_ea(ea); CYC(5); if (DP_OFF) CYC(1); CYC(1); break; } \
	case (base|0x12): { uint8_t _dp = F(); ea = indp(_CPUCA _dp); op_ea(ea); CYC(5); if (DP_OFF) CYC(1); break; } \
	case (base|0x15): { uint8_t _dp = F(); ea = dpx(_CPUCA _dp); op_ea_w(ea); CYC(4); if (DP_OFF) CYC(1); break; } \
	case (base|0x19): { uint16_t _a = F16(); ea = absy(_CPUCA _a); op_ea(ea); CYC(4); CYC(1); break; } \
	case (base|0x1D): { uint16_t _a = F16(); ea = absx(_CPUCA _a); op_ea(ea); CYC(4); CYC(1); break; } \
	case (base|0x1F): { F24(); ea = absxl(_CPUCA ea); op_ea(ea); CYC(5); break; } \
	case (base|0x03): { uint8_t _off = F(); ea = sr(_CPUCA _off); op_ea_w(ea); CYC(4); break; } \
	case (base|0x13): { uint8_t _off = F(); ea = sriy(_CPUCA _off); op_ea(ea); CYC(7); break; } \
	case (base|0x07): { uint8_t _dp = F(); ea = indpl(_CPUCA _dp); op_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x17): { uint8_t _dp = F(); ea = indly(_CPUCA _dp); op_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x0F): { F24(); op_ea(ea); CYC(5); break; }

/* 14 addressing modes for store operations (STA — no immediate mode) */
#define STORE_MODES(base, st_ea, st_ea_w) \
	case (base|0x01): { uint8_t _dp = F(); ea = indx(_CPUCA _dp); st_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x05): { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); st_ea_w(ea); CYC(3); if (DP_OFF) CYC(1); break; } \
	case (base|0x0D): { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); st_ea(ea); CYC(4); break; } \
	case (base|0x11): { uint8_t _dp = F(); ea = indy(_CPUCA _dp); st_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x12): { uint8_t _dp = F(); ea = indp(_CPUCA _dp); st_ea(ea); CYC(5); if (DP_OFF) CYC(1); break; } \
	case (base|0x15): { uint8_t _dp = F(); ea = dpx(_CPUCA _dp); st_ea_w(ea); CYC(4); if (DP_OFF) CYC(1); break; } \
	case (base|0x19): { uint16_t _a = F16(); ea = absy(_CPUCA _a); st_ea(ea); CYC(5); break; } \
	case (base|0x1D): { uint16_t _a = F16(); ea = absx(_CPUCA _a); st_ea(ea); CYC(5); break; } \
	case (base|0x0F): { F24(); st_ea(ea); CYC(5); break; } \
	case (base|0x1F): { F24(); st_ea(absxl(_CPUCA ea)); CYC(5); break; } \
	case (base|0x03): { uint8_t _off = F(); ea = sr(_CPUCA _off); st_ea_w(ea); CYC(4); break; } \
	case (base|0x13): { uint8_t _off = F(); ea = sriy(_CPUCA _off); st_ea(ea); CYC(7); break; } \
	case (base|0x07): { uint8_t _dp = F(); ea = indpl(_CPUCA _dp); st_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x17): { uint8_t _dp = F(); ea = indly(_CPUCA _dp); st_ea(ea); CYC(6); if (DP_OFF) CYC(1); break; }

/* ADC/SBC helpers */
#define DO_ADC_8(Ao, val) do { \
	uint8_t _ao = (uint8_t)(Ao); uint8_t _vl = (uint8_t)(val); \
	uint8_t _ci = (P & P65816_C) ? 1 : 0; \
	if (P & P65816_D) { \
		int16_t _lo = (_ao & 0x0F) + (_vl & 0x0F) + _ci; \
		int16_t _lo_carry = _lo > 9; \
		if (_lo > 9) _lo += 6; \
		int16_t _hi = (_ao >> 4) + (_vl >> 4) + _lo_carry; \
		uint8_t _bcdco = _hi > 9; \
		int16_t _inter = (_hi << 4) | (_lo & 0x0F); \
		if ((~(_ao ^ _vl) & (_ao ^ _inter) & 0x80)) P |= P65816_V; else P &= ~P65816_V; \
		if (_hi > 9) _hi += 6; \
		uint8_t _bcdr = (uint8_t)((_hi & 0x0F) << 4) | (_lo & 0x0F); \
		SETC(_bcdco); \
		A = (A & 0xFF00) | _bcdr; \
		SETNZ8(_bcdr); \
	} else { \
		uint16_t t2 = (uint16_t)_ao + (uint16_t)_vl + _ci; \
		if ((~(_ao ^ _vl) & (_ao ^ t2) & 0x80)) P |= P65816_V; else P &= ~P65816_V; \
		SETC(t2 > 0xFF); \
		A = (A & 0xFF00) | (t2 & 0xFF); \
		SETNZ8(A & 0xFF); \
	} \
} while(0)

#define DO_ADC_16(val) do { \
	uint16_t _vl16 = (uint16_t)(val); \
	uint16_t _ci = (P & P65816_C) ? 1 : 0; \
	if (P & P65816_D) { \
		uint8_t alo = A & 0xFF, vlo = _vl16 & 0xFF; \
		int16_t al = (alo & 0x0F) + (vlo & 0x0F) + _ci; \
		int16_t ah_carry = al > 9; \
		if (al > 9) al += 6; \
		int16_t ah = (alo >> 4) + (vlo >> 4) + ah_carry; \
		uint8_t carry_lo = ah > 9; \
		if (ah > 9) ah += 6; \
		uint8_t lo = (uint8_t)((ah & 0x0F) << 4) | (al & 0x0F); \
		uint8_t ahi = (A >> 8) & 0xFF, vhi = (_vl16 >> 8) & 0xFF; \
		int16_t bl = (ahi & 0x0F) + (vhi & 0x0F) + carry_lo; \
		int16_t bh_carry = bl > 9; \
		if (bl > 9) bl += 6; \
		int16_t bh = (ahi >> 4) + (vhi >> 4) + bh_carry; \
		uint8_t carry_hi = bh > 9; \
		int16_t _inter_hi = (bh << 4) | (bl & 0x0F); \
		if ((~(A ^ _vl16) & (A ^ (((uint32_t)_inter_hi << 8) | lo)) & 0x8000)) P |= P65816_V; else P &= ~P65816_V; \
		if (bh > 9) bh += 6; \
		uint16_t result = (uint16_t)(((bh & 0x0F) << 4) | (bl & 0x0F)) << 8 | lo; \
		SETC(carry_hi); \
		A = result; \
		SETNZ16(A); \
	} else { \
		uint32_t t2 = A + _vl16 + _ci; \
		if ((~(A ^ _vl16) & (A ^ t2) & 0x8000)) P |= P65816_V; else P &= ~P65816_V; \
		SETC(t2 > 0xFFFF); \
		A = t2 & 0xFFFF; \
		SETNZ16(A); \
	} \
} while(0)

#define DO_SBC_8(Ao, val) do { \
	uint8_t _ao = (uint8_t)(Ao); uint8_t _vl = (uint8_t)(val); \
	uint8_t _ci = (P & P65816_C) ? 0 : 1; \
	if (P & P65816_D) { \
		int16_t lo_ = (_ao & 0x0F) - (_vl & 0x0F) - _ci; \
		int16_t borrow_lo_ = lo_ < 0; \
		if (borrow_lo_) lo_ = ((lo_ - 6) & 0x0F); \
		int16_t hi_ = (_ao >> 4) - (_vl >> 4) - borrow_lo_; \
		uint8_t borrow_hi_ = hi_ < 0; \
		if (borrow_hi_) hi_ = ((hi_ - 6) & 0x0F); \
		uint8_t result_ = ((hi_ & 0x0F) << 4) | (lo_ & 0x0F); \
		if (((_ao ^ _vl) & (_ao ^ ((uint16_t)_ao - (uint16_t)_vl - _ci)) & 0x80)) P |= P65816_V; else P &= ~P65816_V; \
		SETC((int16_t)_ao - (int16_t)_vl - _ci >= 0); \
		A = (A & 0xFF00) | result_; \
		SETNZ8(result_); \
	} else { \
		uint16_t t2 = (uint16_t)_ao - (uint16_t)_vl - _ci; \
		if (((_ao ^ _vl) & 0x80) && ((_ao ^ (t2 & 0xFF)) & 0x80)) P |= P65816_V; else P &= ~P65816_V; \
		SETC(t2 < 0x100); \
		A = (A & 0xFF00) | (t2 & 0xFF); \
		SETNZ8(A & 0xFF); \
	} \
} while(0)

#define DO_SBC_16(val) do { \
	uint16_t _vl16 = (uint16_t)(val); \
	uint16_t _ci = (P & P65816_C) ? 0 : 1; \
	if (P & P65816_D) { \
		uint8_t alo = A & 0xFF, vlo = _vl16 & 0xFF; \
		int16_t lo_ = (alo & 0x0F) - (vlo & 0x0F) - _ci; \
		int16_t borrow_lo_ = lo_ < 0; \
		if (borrow_lo_) lo_ = ((lo_ - 6) & 0x0F); \
		int16_t hi_ = (alo >> 4) - (vlo >> 4) - borrow_lo_; \
		uint8_t borrow_byte_ = hi_ < 0; \
		if (borrow_byte_) hi_ = ((hi_ - 6) & 0x0F); \
		uint8_t lo_result = ((hi_ & 0x0F) << 4) | (lo_ & 0x0F); \
		uint8_t ahi = (A >> 8) & 0xFF, vhi = (_vl16 >> 8) & 0xFF; \
		int16_t blo = (ahi & 0x0F) - (vhi & 0x0F) - borrow_byte_; \
		int16_t borrow_blo = blo < 0; \
		if (borrow_blo) blo = ((blo - 6) & 0x0F); \
		int16_t bhi = (ahi >> 4) - (vhi >> 4) - borrow_blo; \
		if (bhi < 0) bhi = ((bhi - 6) & 0x0F); \
		uint8_t hi_result = ((bhi & 0x0F) << 4) | (blo & 0x0F); \
		uint16_t result_ = ((uint16_t)hi_result << 8) | lo_result; \
		uint32_t _bin16 = (uint32_t)A - (uint32_t)_vl16 - _ci; \
		if (((A ^ _vl16) & (A ^ _bin16) & 0x8000)) P |= P65816_V; else P &= ~P65816_V; \
		SETC((int32_t)A - (int32_t)_vl16 - _ci >= 0); \
		A = result_; \
		SETNZ16(A); \
	} else { \
		uint32_t t2 = A - _vl16 - _ci; \
		if (((A ^ _vl16) & 0x8000) && ((A ^ t2) & 0x8000)) P |= P65816_V; else P &= ~P65816_V; \
		SETC(t2 < 0x10000); \
		A = t2 & 0xFFFF; \
		SETNZ16(A); \
	} \
} while(0)

/* ADC/SBC dispatch macros for ALU_MODES */
#define ADC_EA(ea_val) do { if (M8) { uint8_t _v = rd8(_CPUCA (ea_val)); DO_ADC_8(A & 0xFF, _v); } else { uint16_t _v = rd16(_CPUCA (ea_val)); DO_ADC_16(_v); } } while(0)
#define ADC_EA_W(ea_val) do { if (M8) { uint8_t _v = rd8(_CPUCA (ea_val)); DO_ADC_8(A & 0xFF, _v); } else { uint16_t _v = rd16_wrap(_CPUCA (ea_val)); DO_ADC_16(_v); } } while(0)
#define SBC_EA(ea_val) do { if (M8) { uint8_t _v = rd8(_CPUCA (ea_val)); DO_SBC_8(A & 0xFF, _v); } else { uint16_t _v = rd16(_CPUCA (ea_val)); DO_SBC_16(_v); } } while(0)
#define SBC_EA_W(ea_val) do { if (M8) { uint8_t _v = rd8(_CPUCA (ea_val)); DO_SBC_8(A & 0xFF, _v); } else { uint16_t _v = rd16_wrap(_CPUCA (ea_val)); DO_SBC_16(_v); } } while(0)

/* Shift/Rotate/INC/DEC memory macros */
#define _ASL_EA(ea_val, rd16_fn, wr16_fn) do { \
	if (M8) { uint8_t v = rd8(_CPUCA (ea_val)); SETC(v & 0x80); v <<= 1; wr8(_CPUCA (ea_val), v); SETNZ8(v); } \
	else { uint16_t v = rd16_fn(_CPUCA (ea_val)); SETC(v & 0x8000); v <<= 1; wr16_fn(_CPUCA (ea_val), v); SETNZ16(v); } \
} while(0)
#define _LSR_EA(ea_val, rd16_fn, wr16_fn) do { \
	if (M8) { uint8_t v = rd8(_CPUCA (ea_val)); SETC(v & 1); v >>= 1; wr8(_CPUCA (ea_val), v); SETNZ8(v); } \
	else { uint16_t v = rd16_fn(_CPUCA (ea_val)); SETC(v & 1); v >>= 1; wr16_fn(_CPUCA (ea_val), v); SETNZ16(v); } \
} while(0)
#define _ROL_EA(ea_val, rd16_fn, wr16_fn) do { \
	if (M8) { uint8_t v = rd8(_CPUCA (ea_val)); uint8_t c = P & P65816_C; SETC(v & 0x80); v = (v << 1) | c; wr8(_CPUCA (ea_val), v); SETNZ8(v); } \
	else { uint16_t v = rd16_fn(_CPUCA (ea_val)); uint16_t c = P & P65816_C; SETC(v & 0x8000); v = (v << 1) | c; wr16_fn(_CPUCA (ea_val), v); SETNZ16(v); } \
} while(0)
#define _ROR_EA(ea_val, rd16_fn, wr16_fn) do { \
	if (M8) { uint8_t v = rd8(_CPUCA (ea_val)); uint8_t c = P & P65816_C; SETC(v & 1); v = (v >> 1) | (c << 7); wr8(_CPUCA (ea_val), v); SETNZ8(v); } \
	else { uint16_t v = rd16_fn(_CPUCA (ea_val)); uint16_t c = P & P65816_C; SETC(v & 1); v = (v >> 1) | (c << 15); wr16_fn(_CPUCA (ea_val), v); SETNZ16(v); } \
} while(0)
#define _INC_DEC_EA(ea_val, op, rd16_fn, wr16_fn) do { \
	if (M8) { uint8_t v = rd8(_CPUCA (ea_val)) op 1; wr8(_CPUCA (ea_val), v); SETNZ8(v); } \
	else { uint16_t v = rd16_fn(_CPUCA (ea_val)) op 1; wr16_fn(_CPUCA (ea_val), v); SETNZ16(v); } \
} while(0)

/* Shift/Rotate/INC/DEC addressing mode dispatch.
 * Wraps the 4 memory addressing modes (dp, abs, dp,X, abs,X).
 * _W variants use rd16_wrap/wr16_wrap for DP bank-0 wrapping. */
#define _SHIFT_MODES(base, op_w, op) \
	case (base|0x06): { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); op_w; CYC(5); if (DP_OFF) CYC(1); break; } \
	case (base|0x0E): { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); op; CYC(6); break; } \
	case (base|0x16): { uint8_t _dp = F(); ea = dpx(_CPUCA _dp); op_w; CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x1E): { uint16_t _a = F16(); ea = absx(_CPUCA _a); op; CYC(7); break; }

#define _INCDEC_MODES(base, op_w, op) \
	case (base|0x06): { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); op_w; CYC(5); if (DP_OFF) CYC(1); break; } \
	case (base|0x0E): { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); op; CYC(6); break; } \
	case (base|0x16): { uint8_t _dp = F(); ea = dpx(_CPUCA _dp); op_w; CYC(6); if (DP_OFF) CYC(1); break; } \
	case (base|0x1E): { uint16_t _a = F16(); ea = absx(_CPUCA _a); op; CYC(7); break; }

/* Wrapper macros to avoid comma-in-macro-arg issue */
#define _DO_ASL_W _ASL_EA(ea, rd16_wrap, wr16_wrap)
#define _DO_ASL _ASL_EA(ea, rd16, wr16)
#define _DO_LSR_W _LSR_EA(ea, rd16_wrap, wr16_wrap)
#define _DO_LSR _LSR_EA(ea, rd16, wr16)
#define _DO_ROL_W _ROL_EA(ea, rd16_wrap, wr16_wrap)
#define _DO_ROL _ROL_EA(ea, rd16, wr16)
#define _DO_ROR_W _ROR_EA(ea, rd16_wrap, wr16_wrap)
#define _DO_ROR _ROR_EA(ea, rd16, wr16)
#define _DO_INC_W _INC_DEC_EA(ea, +, rd16_wrap, wr16_wrap)
#define _DO_INC _INC_DEC_EA(ea, +, rd16, wr16)
#define _DO_DEC_W _INC_DEC_EA(ea, -, rd16_wrap, wr16_wrap)
#define _DO_DEC _INC_DEC_EA(ea, -, rd16, wr16)

/* TSB/TRB: test and set/reset bits. Pass write value expressions for 8/16-bit */
#define _TSB_TRB_EA(ea_val, rd16_fn, wr16_fn, write8, write16) do { \
	if (M8) { \
		uint8_t v = rd8(_CPUCA (ea_val)); \
		wr8(_CPUCA (ea_val), write8); \
		P = (P & ~P65816_Z) | ((v & (A & 0xFF)) ? 0 : P65816_Z); \
	} else { \
		uint16_t v = rd16_fn(_CPUCA (ea_val)); \
		wr16_fn(_CPUCA (ea_val), write16); \
		P = (P & ~P65816_Z) | ((v & A) ? 0 : P65816_Z); \
	} \
} while(0)

/* Init/Reset/IRQ/NMI */
void cpu65816_init(_CPUC uint8_t* ram_ptr, cpu65816_read_cb rd_cb, cpu65816_write_cb wr_cb) {
	ram = ram_ptr;
#ifdef MEM_IO
	mem_read = rd_cb; mem_write = wr_cb;
#ifndef NO_IO_MAP
	memset(io_map, 0, sizeof(io_map));
#endif
#else
	(void)rd_cb; (void)wr_cb;
#endif
	cpu65816_reset(_CPUPA);
}

#if defined(MEM_IO) && !defined(NO_IO_MAP)
void cpu65816_io_set(_CPUC uint16_t page, uint8_t val) { io_map[page] = val; }
void cpu65816_io_range(_CPUC uint16_t lo, uint16_t hi) { while (lo <= hi) io_map[lo++] = 1; }
#endif

void cpu65816_reset(_CPUP) {
	PBR = 0; DBR = 0; D = 0; A = 0; X = 0; Y = 0;
	P = P65816_M | P65816_X | P65816_I | 0x20;
	SP = INIT_SP_N; E = 1; halted = 0;
#ifdef COUNT_CYCLES
	waiting = 0;
#endif
	PC = rd16(_CPUCA RST_VEC_E);
}

#ifdef SINGLE_INST
static CPU65816* g_cpu;
void cpu65816_set(CPU65816* cpu) {
	g_cpu = cpu;
	A = cpu->A; X = cpu->X; Y = cpu->Y; SP = cpu->SP; PC = cpu->PC;
	D = cpu->D; P = cpu->P; PBR = cpu->PBR; DBR = cpu->DBR; E = cpu->E;
	halted = cpu->halted;
	ctx = cpu->ctx;
#ifdef COUNT_CYCLES
	waiting = cpu->waiting;
#endif
#ifdef MEM_IO
	mem_read = cpu->mem_read; mem_write = cpu->mem_write;
#endif
}
void cpu65816_get(void) {
	CPU65816* cpu = g_cpu;
	cpu->A = A; cpu->X = X; cpu->Y = Y; cpu->SP = SP; cpu->PC = PC;
	cpu->D = D; cpu->P = P; cpu->PBR = PBR; cpu->DBR = DBR; cpu->E = E;
	cpu->halted = halted;
	cpu->ctx = ctx;
#ifdef COUNT_CYCLES
	cpu->waiting = waiting;
#endif
#ifdef MEM_IO
	cpu->mem_read = mem_read; cpu->mem_write = mem_write;
#endif
}
const CPU65816* cpu65816_ptr(void) { return g_cpu; }
#endif

/* Interrupt push+vector: common to IRQ, NMI, BRK, COP */
static void int_push_vector(_CPUC uint16_t vec_e, uint16_t vec_n) {
	if (E) {
		PUSH8((PC >> 8) & 0xFF);
		PUSH8(PC & 0xFF);
		PUSH8(P | 0x20);
		P |= P65816_I;
		PBR = 0;
		PC = rd16(_CPUCA vec_e);
	}
	else {
		PUSH8(PBR);
		PUSH8((PC >> 8) & 0xFF);
		PUSH8(PC & 0xFF);
		PUSH8(P);
		P |= P65816_I;
		PBR = 0;
		PC = rd16(_CPUCA vec_n);
	}
}

void cpu65816_irq(_CPUC BOOL force) {
	if (!force && (P & P65816_I)) return;
#ifdef COUNT_CYCLES
	waiting = 0;
#endif
	int_push_vector(_CPUCA IRQ_VEC_E, IRQ_VEC_N);
}

void cpu65816_nmi(_CPUP) {
#ifdef COUNT_CYCLES
	waiting = 0;
#endif
	int_push_vector(_CPUCA NMI_VEC_E, NMI_VEC_N);
}

#ifdef DEBUG
void cpu65816_dump(_CPUP) {
	printf("A=%04X X=%04X Y=%04X SP=%04X PC=%02X:%04X D=%04X DB=%02X P=%02X E=%d %c%c%c%c%c%c%c%c\n",
		A, X, Y, SP, PBR, PC, D, DBR, P, E,
		P & P65816_N ? 'N' : 'n', P & P65816_V ? 'V' : 'v', P & P65816_M ? 'M' : 'm', P & P65816_X ? 'X' : 'x',
		P & P65816_D ? 'D' : 'd', P & P65816_I ? 'I' : 'i', P & P65816_Z ? 'Z' : 'z', P & P65816_C ? 'C' : 'c');
}
#endif

BOOL cpu65816_is_halted(_CPUP) { return halted; }

/* Step */
CYCLES cpu65816_step(_CPUP) {
	STEPSTART;

	if (halted) return STEPRET;
#ifdef COUNT_CYCLES
	if (waiting) return STEPRET;
#endif

	uint8_t _fb;
	uint16_t _fb16;
	uint16_t _pull16_tmp;
	uint32_t ea;
	(void)_fb; (void)_fb16; (void)_pull16_tmp; (void)ea;

	uint8_t op = F();
	switch (op) {

	/* BRK/COP: fetch signature byte, push state, jump to vector */
#define INT_SW(vec_e, vec_n) do { \
	uint8_t sig = F(); (void)sig; \
	if (E) { \
		PUSH8((PC >> 8) & 0xFF); PUSH8(PC & 0xFF); PUSH8(P | 0x30); \
		P |= P65816_I; P &= ~P65816_D; PBR = 0; \
		PC = rd16(_CPUCA vec_e); CYC(7); \
	} else { \
		PUSH8(PBR); PUSH8((PC >> 8) & 0xFF); PUSH8(PC & 0xFF); PUSH8(P); \
		P |= P65816_I; P &= ~P65816_D; PBR = 0; \
		PC = rd16(_CPUCA vec_n); CYC(8); \
	} \
} while(0)

	case 0x00: INT_SW(IRQ_VEC_E, BRK_VEC_N); break; /* BRK $00 */
	case 0x02: INT_SW(COP_VEC_E, COP_VEC_N); break; /* COP $02 */

	/* WDM $42 — 2-byte NOP (reserved) */
	case 0x42: { uint8_t dummy = F(); (void)dummy; CYC(2); break; }

	/* RTI / RTS / RTL */
	case 0x40: { /* RTI */
		if (E) {
			P = PULL8() | 0x20; PC = PULL16(); CYC(7);
		} else {
			P = PULL8(); PC = PULL16(); PBR = PULL8();
			if (P & P65816_X) { X &= 0xFF; Y &= 0xFF; }
			CYC(7);
		}
		break;
	}
	case 0x60: PC = PULL16(); PC++; CYC(6); break; /* RTS */
	case 0x6B: { /* RTL */
		PC = PULL16(); PBR = PULL8(); PC++; CYC(6); break;
	}

	/* JMP / JML / JSR / JSL */
	case 0x4C: PC = F16(); CYC(3); break; /* JMP abs */
	case 0x6C: { /* JMP (abs) — pointer in bank 0, wraps at 64K */
		uint16_t addr = F16();
		PC = rd16_wrap(_CPUCA (uint32_t)addr);
		CYC(5); break;
	}
	case 0x7C: { /* JMP (abs,X) — pointer in program bank, wraps at 64K */
		uint16_t addr = F16();
		uint16_t idx = X8 ? (X & 0xFF) : X;
		uint32_t ea = ((uint32_t)PBR << 16) | (uint16_t)(addr + idx);
		PC = rd16_wrap(_CPUCA ea);
		CYC(6); break;
	}
	case 0x5C: { /* JML / JMP long */
		F24();
		PBR = (ea >> 16) & 0xFF; PC = ea & 0xFFFF;
		CYC(4); break;
	}
	case 0xDC: { /* JML [abs] — jump indirect long, bank 0 */
		uint16_t addr = F16();
		uint32_t target = rd8(_CPUCA (uint32_t)addr) | ((uint16_t)rd8(_CPUCA (uint32_t)((addr + 1) & 0xFFFF)) << 8)
			| ((uint32_t)rd8(_CPUCA (uint32_t)((addr + 2) & 0xFFFF)) << 16);
		PBR = (target >> 16) & 0xFF; PC = target & 0xFFFF;
		CYC(6); break;
	}
	case 0x20: { /* JSR abs */
		uint16_t addr = F16();
		uint16_t ret = PC - 1;
		PUSH8((ret >> 8) & 0xFF); PUSH8(ret & 0xFF);
		PC = addr; CYC(6); break;
	}
	case 0x22: { /* JSL / JSR long */
		F24();
		PUSH8(PBR);
		uint16_t ret = PC - 1;
		PUSH8((ret >> 8) & 0xFF); PUSH8(ret & 0xFF);
		PBR = (ea >> 16) & 0xFF; PC = ea & 0xFFFF;
		CYC(8); break;
	}
	case 0xFC: { /* JSR (abs,X) — absolute indexed indirect */
		uint16_t addr = F16();
		uint16_t idx = X8 ? (X & 0xFF) : X;
		uint32_t ea_full = ((uint32_t)PBR << 16) | (uint16_t)(addr + idx);
		uint16_t ret = PC - 1;
		PUSH8((ret >> 8) & 0xFF); PUSH8(ret & 0xFF);
		PC = rd16_wrap(_CPUCA ea_full);
		CYC(8); break;
	}

	/* Branch instructions */
#define BRANCH8(cond) do { \
	int8_t off = (int8_t)F(); \
	if (cond) { uint16_t old_pc = PC; PC = (uint16_t)(PC + off); CYC(3); \
		if (E && ((old_pc ^ PC) & 0xFF00)) CYC(1); } else CYC(2); \
} while(0)

	case 0x10: BRANCH8(!(P & P65816_N)); break; /* BPL */
	case 0x30: BRANCH8(P & P65816_N); break; /* BMI */
	case 0x50: BRANCH8(!(P & P65816_V)); break; /* BVC */
	case 0x70: BRANCH8(P & P65816_V); break; /* BVS */
	case 0x90: BRANCH8(!(P & P65816_C)); break; /* BCC */
	case 0xB0: BRANCH8(P & P65816_C); break; /* BCS */
	case 0xD0: BRANCH8(!(P & P65816_Z)); break; /* BNE */
	case 0xF0: BRANCH8(P & P65816_Z); break; /* BEQ */
	case 0x80: { /* BRA */
		int8_t off = (int8_t)F();
		PC = (uint16_t)(PC + off);
		CYC(3); break;
	}
	case 0x82: { /* BRL */
		uint16_t off = F16();
		PC = (uint16_t)(PC + (int16_t)off);
		CYC(4); break;
	}

	/* Flag instructions */
	case 0x18: P &= ~P65816_C; CYC(2); break; /* CLC */
	case 0x38: P |= P65816_C; CYC(2); break; /* SEC */
	case 0x58: P &= ~P65816_I; CYC(2); break; /* CLI */
	case 0x78: P |= P65816_I; CYC(2); break; /* SEI */
	case 0xB8: P &= ~P65816_V; CYC(2); break; /* CLV */
	case 0xD8: P &= ~P65816_D; CYC(2); break; /* CLD */
	case 0xF8: P |= P65816_D; CYC(2); break; /* SED */
	case 0xC2: { /* REP */
		uint8_t mask = F();
		P &= ~mask;
		if (E) P |= P65816_M | P65816_X;
		if (P & P65816_X) { X &= 0xFF; Y &= 0xFF; }
		CYC(3); break;
	}
	case 0xE2: { /* SEP */
		uint8_t mask = F();
		P |= mask;
		if (E) P |= P65816_M | P65816_X;
		if (P & P65816_X) { X &= 0xFF; Y &= 0xFF; }
		CYC(3); break;
	}

	/* Transfer instructions */
	case 0xAA: if (X8) { X = (X & 0xFF00) | (A & 0xFF); SETNZ8(X & 0xFF); } else { X = A; SETNZ16(X); } CYC(2); break; /* TAX */
	case 0xA8: if (X8) { Y = (Y & 0xFF00) | (A & 0xFF); SETNZ8(Y & 0xFF); } else { Y = A; SETNZ16(Y); } CYC(2); break; /* TAY */
	case 0x8A: if (M8) { A = (A & 0xFF00) | (X & 0xFF); SETNZ8(A & 0xFF); } else { A = X8 ? (X & 0xFF) : X; SETNZ16(A); } CYC(2); break; /* TXA */
	case 0x98: if (M8) { A = (A & 0xFF00) | (Y & 0xFF); SETNZ8(A & 0xFF); } else { A = X8 ? (Y & 0xFF) : Y; SETNZ16(A); } CYC(2); break; /* TYA */
	case 0x9A: SP = X; CYC(2); break; /* TXS (X→SP) */
	case 0xBA: if (X8) { X = (X & 0xFF00) | (SP & 0xFF); SETNZ8(SP & 0xFF); } else { X = SP; SETNZ16(X); } CYC(2); break; /* TSX → TBS */
	case 0x9B: Y = X8 ? (X & 0xFF) : X; if (X8) SETNZ8(Y & 0xFF); else SETNZ16(Y); CYC(2); break; /* TXY */
	case 0xBB: X = X8 ? (Y & 0xFF) : Y; if (X8) SETNZ8(X & 0xFF); else SETNZ16(X); CYC(2); break; /* TYX */
	case 0x5B: D = A; SETNZ16(D); CYC(2); break; /* TCD */
	case 0x7B: A = D; SETNZ16(A); CYC(2); break; /* TDC */
	case 0x1B: SP = A; CYC(2); break; /* TCS (alias) */
	case 0x3B: A = SP; SETNZ16(A); CYC(2); break; /* TSC */
	case 0xEB: { /* XBA — swap accumulator bytes */
		uint8_t lo = A & 0xFF;
		A = ((A >> 8) & 0xFF) | (lo << 8);
		SETNZ8(A & 0xFF);
		CYC(3); break;
	}
	case 0xFB: { /* XCE — exchange carry and emulation */
		uint8_t old_e = E;
		E = (P & P65816_C) ? 1 : 0;
		if (old_e) P |= P65816_C; else P &= ~P65816_C;
		if (E) { P |= P65816_M | P65816_X; X &= 0xFF; Y &= 0xFF; SP = 0x0100 | (SP & 0xFF); }
		CYC(2); break;
	}

	/* Stack instructions */
	case 0x08: PUSH8(P | (E ? 0x30 : 0)); CYC(3); break; /* PHP — push P, bits 4&5 set in emulation */
	case 0x68: if (M8) { A = (A & 0xFF00) | PULL8(); SETNZ8(A & 0xFF); } else { A = PULL16(); SETNZ16(A); } CYC(4); break; /* PLA */
	case 0x5A: if (X8) PUSH8(Y & 0xFF); else PUSH16(Y); CYC(3); break; /* PHY */
	case 0x7A: if (X8) { Y = (Y & 0xFF00) | PULL8(); SETNZ8(Y & 0xFF); } else { Y = PULL16(); SETNZ16(Y); } CYC(4); break; /* PLY */
	case 0xDA: if (X8) PUSH8(X & 0xFF); else PUSH16(X); CYC(3); break; /* PHX */
	case 0xFA: if (X8) { X = (X & 0xFF00) | PULL8(); SETNZ8(X & 0xFF); } else { X = PULL16(); SETNZ16(X); } CYC(4); break; /* PLX */
	case 0x0B: PUSH16(D); CYC(4); break; /* PHD */
	case 0x2B: D = PULL16(); SETNZ16(D); CYC(5); break; /* PLD */
	case 0x8B: PUSH8(DBR); CYC(3); break; /* PHB */
	case 0xAB: { uint8_t _pb = PULL8(); DBR = _pb; SETNZ8(_pb); CYC(4); break; } /* PLB */
	case 0x4B: PUSH8(PBR); CYC(3); break; /* PHK */
	case 0x48: if (M8) PUSH8(A & 0xFF); else PUSH16(A); CYC(3); break; /* PHA */
	case 0x28: { /* PLP */
		uint8_t pval = PULL8();
		if (E) pval |= 0x30;
		P = pval;
		if (P & P65816_X) { X &= 0xFF; Y &= 0xFF; }
		CYC(4); break;
	}
	case 0xF4: { /* PEA addr */
		uint16_t addr = F16();
		PUSH8((addr >> 8) & 0xFF); PUSH8(addr & 0xFF);
		CYC(5); break;
	}
	case 0xD4: { /* PEI (dp) */
		uint8_t _dp = F();
		uint16_t addr = rd16_wrap(_CPUCA dp_a(_CPUCA _dp));
		PUSH8((addr >> 8) & 0xFF); PUSH8(addr & 0xFF);
		CYC(6); break;
	}
	case 0x62: { /* PER rel */
		int16_t off = (int16_t)F16();
		uint16_t addr = (uint16_t)(PC + off);
		PUSH8((addr >> 8) & 0xFF); PUSH8(addr & 0xFF);
		CYC(6); break;
	}

	/* ORA */	ALU_MODES(0x00, ORA_EA, ORA_EA_W, ORA_IMM)
	/* AND */	ALU_MODES(0x20, AND_EA, AND_EA_W, AND_IMM)
	/* EOR */	ALU_MODES(0x40, EOR_EA, EOR_EA_W, EOR_IMM)
	/* ADC */	ALU_MODES(0x60, ADC_EA, ADC_EA_W, ADC_IMM)
	/* SBC */	ALU_MODES(0xE0, SBC_EA, SBC_EA_W, SBC_IMM)
	/* CMP */	ALU_MODES(0xC0, CMP_EA, CMP_EA_W, CMP_IMM)
	/* LDA */	ALU_MODES(0xA0, LDA_EA, LDA_EA_W, LDA_IMM)
	/* STA */	STORE_MODES(0x80, STA_EA, STA_EA_W)

	/* LDX / LDY / STX / STY */
	case 0xA2: { /* LDX #imm */
		if (X8) { X = (X & 0xFF00) | F(); SETNZ8(X & 0xFF); CYC(2); }
		else { X = F16(); SETNZ16(X); CYC(3); }
		break;
	}
	case 0xA6: { uint8_t _dp = F(); if (X8) { X = (X & 0xFF00) | rd8(_CPUCA dp_a(_CPUCA _dp)); SETNZ8(X & 0xFF); } else { X = rd16_wrap(_CPUCA dp_a(_CPUCA _dp)); SETNZ16(X); } CYC(3); if (DP_OFF) CYC(1); break; } /* LDX dp */
	case 0xAE: { uint16_t _a = F16(); if (X8) { X = (X & 0xFF00) | rd8(_CPUCA abs_a(_CPUCA _a)); SETNZ8(X & 0xFF); } else { X = rd16(_CPUCA abs_a(_CPUCA _a)); SETNZ16(X); } CYC(4); break; } /* LDX abs */
	case 0xB6: { uint8_t _dp = F(); if (X8) { X = (X & 0xFF00) | rd8(_CPUCA dpy(_CPUCA _dp)); SETNZ8(X & 0xFF); } else { X = rd16_wrap(_CPUCA dpy(_CPUCA _dp)); SETNZ16(X); } CYC(4); if (DP_OFF) CYC(1); break; } /* LDX dp,Y */
	case 0xBE: { uint16_t _a = F16(); if (X8) { X = (X & 0xFF00) | rd8(_CPUCA absy(_CPUCA _a)); SETNZ8(X & 0xFF); } else { X = rd16(_CPUCA absy(_CPUCA _a)); SETNZ16(X); } CYC(4); CYC(1); break; } /* LDX abs,Y */

	case 0xA0: { /* LDY #imm */
		if (X8) { Y = (Y & 0xFF00) | F(); SETNZ8(Y & 0xFF); CYC(2); }
		else { Y = F16(); SETNZ16(Y); CYC(3); }
		break;
	}
	case 0xA4: { uint8_t _dp = F(); if (X8) { Y = (Y & 0xFF00) | rd8(_CPUCA dp_a(_CPUCA _dp)); SETNZ8(Y & 0xFF); } else { Y = rd16_wrap(_CPUCA dp_a(_CPUCA _dp)); SETNZ16(Y); } CYC(3); if (DP_OFF) CYC(1); break; } /* LDY dp */
	case 0xAC: { uint16_t _a = F16(); if (X8) { Y = (Y & 0xFF00) | rd8(_CPUCA abs_a(_CPUCA _a)); SETNZ8(Y & 0xFF); } else { Y = rd16(_CPUCA abs_a(_CPUCA _a)); SETNZ16(Y); } CYC(4); break; } /* LDY abs */
	case 0xB4: { uint8_t _dp = F(); if (X8) { Y = (Y & 0xFF00) | rd8(_CPUCA dpx(_CPUCA _dp)); SETNZ8(Y & 0xFF); } else { Y = rd16_wrap(_CPUCA dpx(_CPUCA _dp)); SETNZ16(Y); } CYC(4); if (DP_OFF) CYC(1); break; } /* LDY dp,X */
	case 0xBC: { uint16_t _a = F16(); if (X8) { Y = (Y & 0xFF00) | rd8(_CPUCA absx(_CPUCA _a)); SETNZ8(Y & 0xFF); } else { Y = rd16(_CPUCA absx(_CPUCA _a)); SETNZ16(Y); } CYC(4); CYC(1); break; } /* LDY abs,X */

	case 0x86: { uint8_t _dp = F(); if (X8) wr8(_CPUCA dp_a(_CPUCA _dp), X & 0xFF); else wr16_wrap(_CPUCA dp_a(_CPUCA _dp), X); CYC(3); if (DP_OFF) CYC(1); break; } /* STX dp */
	case 0x8E: { uint16_t _a = F16(); if (X8) wr8(_CPUCA abs_a(_CPUCA _a), X & 0xFF); else wr16(_CPUCA abs_a(_CPUCA _a), X); CYC(4); break; } /* STX abs */
	case 0x96: { uint8_t _dp = F(); if (X8) wr8(_CPUCA dpy(_CPUCA _dp), X & 0xFF); else wr16_wrap(_CPUCA dpy(_CPUCA _dp), X); CYC(4); if (DP_OFF) CYC(1); break; } /* STX dp,Y */
	case 0x84: { uint8_t _dp = F(); if (X8) wr8(_CPUCA dp_a(_CPUCA _dp), Y & 0xFF); else wr16_wrap(_CPUCA dp_a(_CPUCA _dp), Y); CYC(3); if (DP_OFF) CYC(1); break; } /* STY dp */
	case 0x8C: { uint16_t _a = F16(); if (X8) wr8(_CPUCA abs_a(_CPUCA _a), Y & 0xFF); else wr16(_CPUCA abs_a(_CPUCA _a), Y); CYC(4); break; } /* STY abs */
	case 0x94: { uint8_t _dp = F(); if (X8) wr8(_CPUCA dpx(_CPUCA _dp), Y & 0xFF); else wr16_wrap(_CPUCA dpx(_CPUCA _dp), Y); CYC(4); if (DP_OFF) CYC(1); break; } /* STY dp,X */

	/* STZ — Store Zero */
	case 0x64: { uint8_t _dp = F(); if (M8) wr8(_CPUCA dp_a(_CPUCA _dp), 0); else wr16_wrap(_CPUCA dp_a(_CPUCA _dp), 0); CYC(3); if (DP_OFF) CYC(1); break; }
	case 0x74: { uint8_t _dp = F(); if (M8) wr8(_CPUCA dpx(_CPUCA _dp), 0); else wr16_wrap(_CPUCA dpx(_CPUCA _dp), 0); CYC(4); if (DP_OFF) CYC(1); break; }
	case 0x9C: { uint16_t _a = F16(); if (M8) wr8(_CPUCA abs_a(_CPUCA _a), 0); else wr16(_CPUCA abs_a(_CPUCA _a), 0); CYC(4); break; }
	case 0x9E: { uint16_t _a = F16(); if (M8) wr8(_CPUCA absx(_CPUCA _a), 0); else wr16(_CPUCA absx(_CPUCA _a), 0); CYC(5); break; }

	/* CPX / CPY */
	case 0xE0: CMP_XY_IMM(X); break; /* CPX #imm */
	case 0xE4: { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); CMP_XY_MEM(X, ea, rd16_wrap); CYC(3); if (DP_OFF) CYC(1); break; }
	case 0xEC: { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); CMP_XY_MEM(X, ea, rd16); CYC(4); break; }
	case 0xC0: CMP_XY_IMM(Y); break; /* CPY #imm */
	case 0xC4: { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); CMP_XY_MEM(Y, ea, rd16_wrap); CYC(3); if (DP_OFF) CYC(1); break; }
	case 0xCC: { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); CMP_XY_MEM(Y, ea, rd16); CYC(4); break; }

	/* INC/DECA, INX/DEX, INY/DEY */
	case 0x1A: if (M8) { A = (A & 0xFF00) | ((A + 1) & 0xFF); SETNZ8(A & 0xFF); } else { A++; SETNZ16(A); } CYC(2); break;
	case 0x3A: if (M8) { A = (A & 0xFF00) | ((A - 1) & 0xFF); SETNZ8(A & 0xFF); } else { A--; SETNZ16(A); } CYC(2); break;
	case 0xE8: if (X8) { X = (X & 0xFF00) | ((X + 1) & 0xFF); SETNZ8(X & 0xFF); } else { X++; SETNZ16(X); } CYC(2); break;
	case 0xCA: if (X8) { X = (X & 0xFF00) | ((X - 1) & 0xFF); SETNZ8(X & 0xFF); } else { X--; SETNZ16(X); } CYC(2); break;
	case 0xC8: if (X8) { Y = (Y & 0xFF00) | ((Y + 1) & 0xFF); SETNZ8(Y & 0xFF); } else { Y++; SETNZ16(Y); } CYC(2); break;
	case 0x88: if (X8) { Y = (Y & 0xFF00) | ((Y - 1) & 0xFF); SETNZ8(Y & 0xFF); } else { Y--; SETNZ16(Y); } CYC(2); break;

	/* Shift/Rotate on memory */
	/* ASL */
	_SHIFT_MODES(0x00, _DO_ASL_W, _DO_ASL)
	case 0x0A: if (M8) { SETC(A & 0x80); A = (A & 0xFF00) | ((A << 1) & 0xFF); SETNZ8(A & 0xFF); } else { SETC(A & 0x8000); A <<= 1; SETNZ16(A); } CYC(2); break;
	/* LSR */
	_SHIFT_MODES(0x40, _DO_LSR_W, _DO_LSR)
	case 0x4A: if (M8) { SETC(A & 1); A = (A & 0xFF00) | ((A >> 1) & 0x7F); SETNZ8(A & 0xFF); } else { SETC(A & 1); A >>= 1; SETNZ16(A); } CYC(2); break;
	/* ROL */
	_SHIFT_MODES(0x20, _DO_ROL_W, _DO_ROL)
	case 0x2A: if (M8) { uint8_t c = P & P65816_C; uint8_t old = A & 0xFF; SETC(old & 0x80); A = (A & 0xFF00) | (((old << 1) | c) & 0xFF); SETNZ8(A & 0xFF); } else { uint16_t c = P & P65816_C; uint16_t old = A; SETC(old & 0x8000); A = (old << 1) | c; SETNZ16(A); } CYC(2); break;
	/* ROR */
	_SHIFT_MODES(0x60, _DO_ROR_W, _DO_ROR)
	case 0x6A: if (M8) { uint8_t c = P & P65816_C; uint8_t old = A & 0xFF; SETC(old & 1); A = (A & 0xFF00) | ((old >> 1) | (c << 7)); SETNZ8(A & 0xFF); } else { uint16_t c = P & P65816_C; uint16_t old = A; SETC(old & 1); A = (old >> 1) | (c << 15); SETNZ16(A); } CYC(2); break;

	/* INC / DEC memory */
	_INCDEC_MODES(0xE0, _DO_INC_W, _DO_INC)
	_INCDEC_MODES(0xC0, _DO_DEC_W, _DO_DEC)

	/* BIT — all modes */
	case 0x89: { /* BIT #imm */
		if (M8) { uint8_t _v = F(); P = (P & ~P65816_Z) | ((A & _v) ? 0 : P65816_Z); CYC(2); }
		else { uint16_t _v = F16(); P = (P & ~P65816_Z) | ((A & _v) ? 0 : P65816_Z); CYC(3); }
		break;
	}
		case 0x24: { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); BIT_EA(ea, rd16_wrap); CYC(3); if (DP_OFF) CYC(1); break; }
		case 0x2C: { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); BIT_EA(ea, rd16); CYC(4); break; }
		case 0x34: { uint8_t _dp = F(); ea = dpx(_CPUCA _dp); BIT_EA(ea, rd16_wrap); CYC(4); if (DP_OFF) CYC(1); break; }
		case 0x3C: { uint16_t _a = F16(); ea = absx(_CPUCA _a); BIT_EA(ea, rd16); CYC(4); CYC(1); break; }

		/* TSB / TRB */
		case 0x04: { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); _TSB_TRB_EA(ea, rd16_wrap, wr16_wrap, v | (A & 0xFF), v | A); CYC(5); if (DP_OFF) CYC(1); break; }
		case 0x0C: { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); _TSB_TRB_EA(ea, rd16, wr16, v | (A & 0xFF), v | A); CYC(6); break; }
		case 0x14: { uint8_t _dp = F(); ea = dp_a(_CPUCA _dp); _TSB_TRB_EA(ea, rd16_wrap, wr16_wrap, v & ~(A & 0xFF), v & ~A); CYC(5); if (DP_OFF) CYC(1); break; }
		case 0x1C: { uint16_t _a = F16(); ea = abs_a(_CPUCA _a); _TSB_TRB_EA(ea, rd16, wr16, v & ~(A & 0xFF), v & ~A); CYC(6); break; }

	/* NOP, WAI, STP */
	case 0xEA: CYC(2); break; /* NOP */
	case 0xCB: { /* WAI */
#ifdef COUNT_CYCLES
		waiting = 1; CYC(3);
#else
		halted = 1;
#endif
		break;
	}
	case 0xDB: halted = 1; CYC(3); break; /* STP */

		/* MVN/MVP: block move. X/Y masked by X flag. MVN inc, MVP dec.
		 Single-step: moves min(A+1,14) bytes per call. A=$FFFF when done. */
#define BLOCK_MOVE(op) do { \
		uint8_t dst = F(); uint8_t src = F(); \
		DBR = dst; \
		{ \
			uint16_t xm = X8 ? 0xFF : 0xFFFF; \
			int count = (int)A + 1; \
			if (count > 14) count = 14; \
			for (int i = 0; i < count; i++) { \
				wr8(_CPUCA ((uint32_t)dst << 16) | (Y & xm), rd8(_CPUCA ((uint32_t)src << 16) | (X & xm))); \
				X = (X op 1) & xm; Y = (Y op 1) & xm; \
			} \
			A = (uint16_t)(A - count); \
			if (A != 0xFFFF) PC--; \
		} \
		CYC(7); \
	} while(0)
		case 0x54: BLOCK_MOVE(+); break; /* MVN src,dst */
		case 0x44: BLOCK_MOVE(-); break; /* MVP dst,src */

	/* Default — unhandled opcode */
	default:
#ifdef DEBUG
		printf("65816: unhandled opcode $%02X at %02X:%04X\n", op, PBR, PC - 1);
#endif
		halted = 1;
		break;
	}

	/* Post-instruction: enforce emulation mode constraints */
	if (E) {
		P |= P65816_M | P65816_X;
		SP = STACK_PAGE_E | (SP & 0xFF);
	}
	if (P & P65816_X) {
		X &= 0xFF;
		Y &= 0xFF;
	}

	return STEPRET;
}

#ifdef COUNT_CYCLES
/* Run up to budget instructions (0 = unlimited). Returns count executed. */
u32 cpu65816_run(_CPUC int budget) {
	u32 count = 0;
	while (budget <= 0 || count < (u32)budget) {
		if (waiting) { cpu65816_step(_CPUPA); count++; continue; }
		if (halted) break;
		cpu65816_step(_CPUPA);
		count++;
	}
	return count;
}
#endif
