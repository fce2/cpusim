/**
 * cpu6502.h - MOS 6502 CPU simulator
 *
 * Build defines:
 *	DEBUG			enable cpu6502_dump() and illegal opcode warnings
 *	SINGLE_INST		bare static globals, no cpu pointer param
 *	MEM_IO			memory r/w callbacks for I/O-mapped hardware
 *	COUNT_CYCLES	step() returns cycle count, else void
 *	ILLEGAL			implement NMOS illegal opcodes: KIL/JAM halts CPU
 *	NO_NZ_TABLE		branch-based SETNZ instead of 256B lookup
 *	NO_IO_MAP		all MEM_IO via callback, no io_map bitmap (saves 256B)
 *
 * CPU variants, default MOS6502:
 *	MOS6502			NMOS (JMP ind page-wrap bug, BCD flags mid-result, the "original")
 *	RICOH2A03		NMOS, no BCD (decimal flag ignored, NES)
 *	ROCKWELL65C02	CMOS (fixed JMP ind, BCD flags from final result)
 *	SYNERTEK65C02	CMOS (RMW illegals → NOPs)
 *	WDC65C02		CMOS + BBR/BBS/RMB/SMB, STP/WAI
 */

#ifndef _CPU6502_H_
#define _CPU6502_H_

#if defined(__cplusplus) && !defined(__OSCAR64C__)
extern "C" {
#endif

#define CPU_TYPE CPU6502
#define cpu_type cpu6502
#define CPU_CYCLES u8
#include "sim.h"

	typedef enum {
		P6502_C = 0x01, /* Carry */
		P6502_Z = 0x02, /* Zero */
		P6502_I = 0x04, /* Interrupt disable */
		P6502_D = 0x08, /* Decimal mode (BCD add/sub on NMOS, NOP on RICOH2A03) */
		P6502_B = 0x10, /* Break — only exists in pushed P during BRK, not a real flag */
		P6502_U = 0x20, /* Unused (always 1 when P is pushed to stack) */
		P6502_V = 0x40, /* Overflow */
		P6502_N = 0x80  /* Negative */
	} P6502_Flags;

	typedef u8 (*cpu6502_read_cb)(_CPUC u16 addr);
	typedef void (*cpu6502_write_cb)(_CPUC u16 addr, u8 val);

#if defined MEM_IO && !defined NO_IO_MAP
	void cpu6502_io_set(_CPUC u8 page, u8 val); /* mark page as I/O (nonzero) or RAM (0) */
	void cpu6502_io_range(_CPUC u8 lo, u8 hi); /* mark page range lo..hi as I/O */
#endif

	struct CPU6502 {
		u8 A;			/* Accumulator */
		u8 X, Y;		/* Index registers */
		u8 P;			/* Processor status: NV-BDIZC */
		u8 SP;			/* Stack pointer (8-bit, offset into page 1: $0100–$01FF) */
		u16 PC;			/* Program counter (16-bit, 64K address space) */
		u8* ram;		/* 64K flat RAM */
		BOOL halted;	/* 'true' after KIL/JAM (NMOS illegal), or STP/WAI (WDC65C02) */
		void* ctx;		/* user context for I/O callbacks */
#ifdef MEM_IO
		cpu6502_read_cb mem_read;
		cpu6502_write_cb mem_write;
#endif
	};

	void	cpu6502_init(_CPUC u8* ram, cpu6502_read_cb rd, cpu6502_write_cb wr);
	void	cpu6502_reset(_CPUP);			/* reset vectors: PC=$FFFC, SP=$FD, I=1 */
	void	cpu6502_irq(_CPUC BOOL force);	/* maskable interrupt (ignored if I=1, unless force) */
	void	cpu6502_nmi(_CPUP);				/* non-maskable interrupt (always taken) */
	BOOL	cpu6502_is_halted(_CPUP);		/* e.g. after KIL */
	CYCLES	cpu6502_step(_CPUP);			/* execute one instruction, return cycle count */
#ifdef COUNT_CYCLES
	u32		cpu6502_run(_CPUC u32 budget);	/* run up to budget instructions */
#endif

#ifdef DEBUG
	void cpu6502_dump(_CCPUP);
#endif

#if defined(__cplusplus) && !defined(__OSCAR64C__)
}
#endif

#endif /* _CPU6502_H_ */
