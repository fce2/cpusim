/**
 * cpu68000.h - Motorola 68000 CPU simulator
 *
 * Build defines:
 *	DEBUG			Log illegal/privilege traps via cpu68000_dump()
 *	SINGLE_INST		Bare static globals, no cpu pointer param (fastest mode)
 *	MEM_IO			Memory r/w callbacks for I/O-mapped hardware
 *	FAST_MEM		Direct ram[] access, no alignment checks (fastest, no fault recovery)
 *	COUNT_CYCLES	step() returns cycle count (u16), else void
 *	BUS_HOOKS		Bus event callback on every rd/wr/fetch (NULL = zero overhead)
 *	PREFETCH		2-word prefetch queue (IRC/IRD), bus-accurate instruction fetch
 *	NO_NZ_TABLE		Branch-based SETNZ instead of 256B lookup (saves memory)
 */

#ifndef _CPU68000_H_
#define _CPU68000_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_TYPE CPU68000
#define cpu_type cpu68000
#define CPU_CYCLES u16
#include "sim.h"

typedef enum {
	SR_C = 0x0001,		/* Carry */
	SR_V = 0x0002,		/* Overflow */
	SR_Z = 0x0004,		/* Zero */
	SR_N = 0x0008,		/* Negative */
	SR_X = 0x0010,		/* Extend */
	SR_IM2 = 0x0100,	/* Interrupt mask bit 2 */
	SR_IM1 = 0x0200,	/* Interrupt mask bit 1 */
	SR_IM0 = 0x0400,	/* Interrupt mask bit 0 */
	SR_S = 0x2000,		/* Supervisor mode */
	SR_T = 0x8000		/* Trace mode */
} SR_Flags;

typedef u8 (*cpu68000_read_cb)(_CPUC u32 addr);
typedef void (*cpu68000_write_cb)(_CPUC u32 addr, u8 val);
typedef int (*cpu68000_intack_cb)(_CPUC int level);

#ifdef BUS_HOOKS
	typedef enum { BUS_RD = 0, BUS_WR = 1, BUS_IF = 2 } bus_op_t;
	typedef struct { bus_op_t op; u32 addr; u32 data; u8 fc; u8 sz; u16 cycles; } bus_event_t;
	typedef int (*cpu68000_bus_cb)(_CPUC bus_event_t *ev);
	#define FC_USER_DATA	1
	#define FC_USER_PROG	2
	#define FC_SUPER_DATA	5
	#define FC_SUPER_PROG	6
#endif

typedef struct CPU68000Fault CPU68000Fault;

struct CPU68000 {
	u32 D[8];			/* Data registers D0-D7 */
	u32 A[8];			/* Address registers A0-A7 (A7=SSP in supervisor) */
	u32 USP;			/* User stack pointer */
	u32 PC;				/* Program counter */
	u16 SR;				/* Status register (S, T, I mask) */
	u32 flag_n, flag_z, flag_v, flag_c, flag_x;
	u8* ram;			/* 16MB flat RAM, assigned by caller */
	cpu68000_read_cb mem_read;	/* ram[] when MEM_IO not active */
	cpu68000_write_cb mem_write;
	cpu68000_intack_cb int_ack;	/* NULL = auto-vector (0x18+level) */
#ifdef BUS_HOOKS
	cpu68000_bus_cb on_bus;	/* NULL = disabled */
	u16 bus_cycle;
	u8 fc;					/* current function code pins */
#endif
	void* ctx;				/* Opaque pointer for callbacks */
	CPU68000Fault* fault;	/* Address error recovery (allocated by init) */
	int ae_fired;			/* 1 if last step triggered address error */
	u8 stopped;				/* 0=running, 1=STOP, 2=double-fault halt */
	u8 fault_depth;			/* Address error nesting depth */
	u8 ipl;					/* External interrupt priority level */
	i8 irq_vector;			/* -1=autovector */
	u8 trace_pending;		/* Trace exception fires after this instruction */
#ifdef PREFETCH
	u16 irc;				/* Prefetch queue: instruction register cache */
	u16 ird;				/* Prefetch queue: instruction register decode */
#endif
#ifdef COUNT_CYCLES
	CPU_CYCLES cycles;		/* Cycle count accumulator (per step) */
#endif
};

void	cpu68000_init(_CPUC u8* ram, cpu68000_read_cb rd, cpu68000_write_cb wr, cpu68000_intack_cb ia);
void	cpu68000_reset(_CPUP);
void	cpu68000_irq(_CPUC int level, int vector);
CYCLES	cpu68000_step(_CPUP);
#ifdef FAST_MEM
	#ifdef COUNT_CYCLES
		u32	cpu68000_run(_CPUC u32 n);
		u32	cpu68000_run_cyc(_CPUC u32 budget);
	#else
		void	cpu68000_run(_CPUC u32 n);
	#endif
#endif

#ifdef DEBUG
	void cpu68000_dump(_CCPUP);
#endif

u16		cpu68000_get_sr(_CCPUP);		/* Reconstructs full 16-bit SR from flag_n/z/v/c/x */
void	cpu68000_set_sr(_CPUC u16 sr);	/* Writes full SR: mask bits to cpu.SR, flags to flag_n/z/v/c/x */
void	cpu68000_set_ipl(_CPUC u8 level, int vector);
size_t	cpu68000_state_size(_CCPUP);
void	cpu68000_save(_CPUC u8* buf);
void	cpu68000_load(_CPUC const u8* buf);

#ifdef __cplusplus
}
#endif

#endif /* _CPU68000_H_ */
