/**
 * cpuZ80.h - Zilog Z80 CPU simulator
 *
 * Build defines:
 *	DEBUG			Enable cpuZ80_dump()
 *	SINGLE_INST		bare static globals, no cpu pointer param
 *	MEM_IO			memory r/w callbacks for I/O-mapped hardware
 *	COUNT_CYCLES	step() returns cycle count (uint8_t), else void
 *	NO_IO_MAP		all MEM_IO via callback, no io_map bitmap
 *	NO_SZP_TABLE	compute SZP flags at runtime, no 256B lookup
 */

#ifndef _CPUZ80_H_
#define _CPUZ80_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_TYPE CPUZ80
#define cpu_type cpuZ80
#define CPU_CYCLES u8
#include "sim.h"

	typedef enum { Z80_SF = 0x80, Z80_ZF = 0x40, Z80_YF = 0x20, Z80_HF = 0x10, Z80_XF = 0x08, Z80_PF = 0x04, Z80_NF = 0x02, Z80_CF = 0x01 } Z80_Flags;

	typedef u8 (*cpuZ80_read_cb)(_CPUC u16 addr);
	typedef void (*cpuZ80_write_cb)(_CPUC u16 addr, u8 val);
	typedef u8 (*cpuZ80_in_cb)(_CPUC u16 port);
	typedef void (*cpuZ80_out_cb)(_CPUC u16 port, u8 val);

#if defined MEM_IO && !defined NO_IO_MAP
	void cpuZ80_io_set(_CPUC u8 page, u8 val);
	void cpuZ80_io_range(_CPUC u8 lo, u8 hi);
#endif

	typedef union { struct { u8 l, h; } b; u16 w; } Reg16;

	struct CPUZ80 {
		Reg16 af, bc, de, hl;	/* .w=16-bit, .b.h/.b.l=high/low (e.g. af.b.h=A, bc.w=BC) */
		Reg16 af_, bc_, de_, hl_;
		Reg16 ix, iy;			/* index registers */
		Reg16 wz;				/* MEMPTR (internal temp) */
		Reg16 sp, pc;
		u8 I, R;				/* interrupt vector, refresh counter */
		u8 IFF1, IFF2, IM;		/* interrupt flip-flops and mode */
		u8 Q;					/* Q flag (SCF/CCF Y/X tracking) */
		u8* ram;				/* 64K flat RAM */
		void* ctx;
		cpuZ80_in_cb port_in;	/* NULL = returns 0xFF */
		cpuZ80_out_cb port_out;	/* NULL = no-op */
#ifdef MEM_IO
		cpuZ80_read_cb mem_read;
		cpuZ80_write_cb mem_write;
#endif
	};

	void	cpuZ80_init(_CPUC u8* ram, cpuZ80_read_cb rd, cpuZ80_write_cb wr, cpuZ80_in_cb pi, cpuZ80_out_cb po);
	void	cpuZ80_reset(_CPUP);
	void	cpuZ80_irq(_CPUP);
	void	cpuZ80_nmi(_CPUP);
	CYCLES	cpuZ80_step(_CPUP);
#ifdef COUNT_CYCLES
	u32		cpuZ80_run(_CPUC u32 budget);
#endif

#ifdef DEBUG
	void cpuZ80_dump(_CCPUP);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _CPUZ80_H_ */
