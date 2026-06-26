/**
 * cpu6502T.h - MOS 6502 transistor-level simulator
 *
 * Drop-in replacement for cpu6502.h. Same struct layout, same init/reset/
 * irq/nmi/step/dump API. Internal implementation is transistor-level
 * event-driven simulation (visual6502 netlist data).
 *
 * Additional API beyond cpu6502.h:
 *   cpu6502T_set()       - safe register write (boot loader method)
 *   cpu6502T_flush()     - settle netlist after step (advances 1 cycle)
 *   cpu6502T_get()       - sync netlist nodes → struct fields
 *   cpu6502T_soft_reset()- lightweight reset (skips build_state)
 *   cpu6502T_half_step() - single half-cycle (phase) for testing
 *   cpu6502T_get/set_node() - read/write arbitrary netlist nodes
 *   cpu6502T_log_start/stop/count/entries() - bus cycle logging
 *   cpu6502T_save/restore_boot() - state snapshots for test generators
 *
 * Build defines:
 *   DEBUG       - enables cpu6502T_dump() and cpu6502T_debug_nodes()
 *   THREADSAFE  - per-thread state (g_state, g_ram) via __thread
 *   COUNT_CYCLES - step() returns cycle count
 *
 * Only NMOS 6502 simulated. Illegal opcodes handled naturally.
 */

#ifndef _CPU6502T_H_
#define _CPU6502T_H_

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_TYPE CPU6502T
#define cpu_type cpu6502T
#define CPU_CYCLES u8
#include "sim.h"

	typedef enum {
		P6502_C = 0x01, /* Carry */
		P6502_Z = 0x02, /* Zero */
		P6502_I = 0x04, /* Interrupt disable */
		P6502_D = 0x08, /* Decimal mode */
		P6502_B = 0x10, /* Break — only in pushed P, not a real flag */
		P6502_U = 0x20, /* Unused (always 1 when P pushed to stack) */
		P6502_V = 0x40, /* Overflow */
		P6502_N = 0x80  /* Negative */
	} P6502_Flags;

	struct CPU6502T {
		u8 A;			/* Accumulator (read-only after step; use cpu6502T_set) */
		u8 X, Y;		/* Index registers (read-only after step; use cpu6502T_set) */
		u8 P;			/* Processor status: NV-BDIZC */
		u8 SP;			/* Stack pointer (8-bit, $0100–$01FF) */
		u16 PC;			/* Program counter */
		u8* ram;		/* 64K flat RAM */
		BOOL halted;	/* true after KIL/JAM */
		void* ctx;		/* user context for I/O callbacks (matches CPU6502) */
	};

	/* ── Core API (matches cpu6502.h) ──────────────────────────────── */
	void	cpu6502T_init(_CPUC u8* ram);
	void	cpu6502T_reset(_CPUP);
	void	cpu6502T_irq(_CPUC BOOL force);
	void	cpu6502T_irq_release(_CPUP);
	void	cpu6502T_nmi(_CPUP);
	CYCLES	cpu6502T_step(_CPUP);
	BOOL	cpu6502T_is_halted(_CCPUP);

#ifdef DEBUG
	void	cpu6502T_dump(_CCPUP);
	void	cpu6502T_debug_nodes(void);
#endif

	/* ── Transistor-level extensions ─────────────────────────────────── */

	/* Safe register set: writes A,X,Y,SP,P,PC via boot loader (LDA/LDX/
	   TXS/LDX/LDY/RTI), then settles all 1725 transistor nodes. Preserves
	   user RAM (vectors, code, stack are saved and restored). */
	void	cpu6502T_set(_CPUP, u16 pc, u8 a, u8 x, u8 y, u8 sp, u8 p);

	/* After step(), some register values (flags, ALU) may not have
	   propagated through the netlist yet. flush() advances 1 cycle to
	   settle; call get() afterwards to sync nodes→struct. */
	void	cpu6502T_flush(_CPUP);
	void	cpu6502T_get(_CPUP);

	/* Lightweight reset: skips build_state, reuse after set() or save/restore. */
	void	cpu6502T_soft_reset(_CPUP);

	/* Single half-cycle (phase) for testing/debugging. */
	void	cpu6502T_half_step(_CPUP);

	/* Read/write arbitrary netlist nodes by index (see netlist_6502.h). */
	u8	cpu6502T_get_node(int node);
	void	cpu6502T_set_node(int node, BOOL val);

	/* ── Bus cycle logging ───────────────────────────────────────────── */

	typedef struct { u16 addr; u8 data; BOOL write; } CPU6502T_Cycle;

	void		cpu6502T_log_start(_CPUP);
	void		cpu6502T_log_stop(_CPUP);
	int		cpu6502T_log_count(_CCPUP);
	const CPU6502T_Cycle *cpu6502T_log_entries(_CCPUP);

	/* ── Boot state save/restore (for test generators) ───────────────── */

	typedef struct CPU6502T_SavedState CPU6502T_SavedState;
	size_t	cpu6502T_saved_state_size(void);
	void	cpu6502T_save_boot(CPU6502T_SavedState *dst);
	void	cpu6502T_restore_boot(const CPU6502T_SavedState *src);

#ifdef __cplusplus
}
#endif

#endif /* _CPU6502T_H_ */