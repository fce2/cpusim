/**
 * cpu65816.h - WDC 65C816 CPU simulator
 *
 * Build defines:
 *	DEBUG			enable cpu65816_dump()
 *	SINGLE_INST		bare static globals, no cpu pointer param
 *	MEM_IO			memory r/w callbacks for I/O-mapped hardware
 *	COUNT_CYCLES	step() returns cycle count, else void
 *	NO_IO_MAP		all MEM_IO via callback, no io_map bitmap (saves 64kb)
 *	RICOH5A22		Ricoh 5A22 (SNES): aliases cpu65816 API as cpu5a22, 
 *					adds DMA register layout to CPU65816 struct
 */

#ifndef _CPU65816_H_
#define _CPU65816_H_

#if defined(__cplusplus) && !defined(__OSCAR64C__)
extern "C" {
#endif

#define CPU_TYPE CPU65816
#define cpu_type cpu65816
#define CPU_CYCLES u8
#include "sim.h"

	typedef enum {
		P65816_C = 0x01, /* Carry */
		P65816_Z = 0x02, /* Zero */
		P65816_I = 0x04, /* Interrupt disable */
		P65816_D = 0x08, /* Decimal mode */
		P65816_X = 0x10, /* Index width (1=8-bit) — native only */
		P65816_M = 0x20, /* Memory/Acc width (1=8-bit) — native only */
		P65816_V = 0x40, /* Overflow */
		P65816_N = 0x80  /* Negative */
	} P65816_Flags;

	typedef u8 (*cpu65816_read_cb)(_CPUC u32 addr);
	typedef void (*cpu65816_write_cb)(_CPUC u32 addr, u8 val);

#if defined(MEM_IO) && !defined(NO_IO_MAP)
	void cpu65816_io_set(_CPUC u16 page, u8 val); /* mark page as I/O (nonzero) or RAM (0); page = addr>>8, 0..65535 */
	void cpu65816_io_range(_CPUC u16 lo, u16 hi); /* mark page range lo..hi as I/O */
#endif

	struct CPU65816 {
		u16 A;		/* Accumulator: low byte = C (8-bit acc), high byte = B */
		u16 X, Y;	/* Index registers (8 or 16-bit depending on X flag) */
		u16 SP;		/* Stack pointer (16-bit native; high byte forced $01 in emu) */
		u16 PC;		/* Program counter (16-bit offset within PBR bank) */
		u16 D;		/* Direct page register */
		u8 P;		/* Processor status: NV-MXDIZC */
		u8 PBR;		/* Program bank register (K) — bits 23-16 of instruction addr */
		u8 DBR;		/* Data bank register — bits 23-16 of data addresses */
		u8 E;		/* Emulation flag: 1=6502-compatible, 0=native */
		u8* ram;	/* 16MB flat RAM (24-bit address space) */
		BOOL halted;/* 1 after STP, or WAI (COUNT_CYCLES mode uses waiting instead) */
		void* ctx;	/* user context for I/O callbacks (e.g. system struct) */
#ifdef MEM_IO
		cpu65816_read_cb mem_read;
		cpu65816_write_cb mem_write;
#endif
#ifdef COUNT_CYCLES
		u8 waiting;	/* WAI stall: 0=running, 1=stalled until irq()/nmi() */
#endif
	};

	void cpu65816_init(_CPUC u8* ram, cpu65816_read_cb rd, cpu65816_write_cb wr);
	void cpu65816_reset(_CPUP);				/* reset: emu mode, SP=$01FF, I=1, M=X=1, PC from $FFFC */
	void cpu65816_irq(_CPUC BOOL force);	/* maskable interrupt (ignored if I=1, unless force) */
	void cpu65816_nmi(_CPUP);				/* non-maskable interrupt (always taken) */
	BOOL cpu65816_is_halted(_CPUP);
	CYCLES cpu65816_step(_CPUP);			/* execute one instruction, return cycle count */
#ifdef COUNT_CYCLES
	u32 cpu65816_run(_CPUC int budget);		/* run up to budget instructions (0=unlimited) */
#endif

#ifdef DEBUG
	void cpu65816_dump(_CCPUP);
#endif

#ifdef RICOH5A22
/* ===================================================================
 * ! WIP !
 * Ricoh 5A22 (SNES CPU) — extends CPU65816 with I/O registers
 *
 * The 5A22 is a 65C816 core with SNES-specific I/O at $4200-$43FF:
 *   $4200  NMITIMEN   NMI/IRQ enable, auto-joypad, fast ROM
 *   $4201  WRIO       programmable I/O port (write-only)
 *   $4202  WRMPYA     multiplicand
 *   $4203  WRMPYB    multiplier (write triggers multiply)
 *   $4204  WRDIVL/H  dividend (16-bit)
 *   $4206  WRDIVB    divisor (write triggers divide)
 *   $4207  HTIMEL/H  H-timer
 *   $4209  VTIMEL/H  V-timer
 *   $420B  MDMAEN    DMA enable (channels 0-7)
 *   $420C  HDMAEN    HDMA enable (channels 0-7)
 *   $420D  MEMSEL    ROM speed: 0=slow (200ns), 1=fast (120ns)
 *   $4210  RDNMI     NMI status (bit7=pending, bit0-6=version 2)
 *   $4211  RDIO/IRQ  IRQ status
 *   $4212  RDHVBJ    H/V-blank, joypad auto
 *   $4214  RDDIVL/H  divide result
 *   $4216  RDMPYL/H  multiply result / divide remainder
 *   $4218  JOY1L/H   joypad 1
 *   $421A  JOY2L/H   joypad 2
 *   $4300  DMA regs  8 channels × 16 bytes each
 *
 * Build with -DRICOH5A22 to include the DMA register layout in
 * CPU65816 and alias the API names (cpu5a22_* → cpu65816_*).
 * =================================================================== */

/* DMA channel registers (8 channels, $43x0-$43xF each) */
typedef struct {
	u8  ctrl;		/* $43x0: direction, HDMA mode, indirect, unused */
	u8  dest;		/* $43x1: destination register ($21xx, low byte only) */
	u16 src;		/* $43x2-$43x3: source address (low 16 bits) */
	u8  bank;		/* $43x4: source bank */
	u16 size;		/* $43x5-$43x6: transfer size / HDMA indirect */
	u8  hdma_bank;	/* $43x7: HDMA indirect bank */
	u8  line;		/* $43x8: HDMA line counter (upper nibble repeat) */
	u8  unused;		/* $43x9: unused */
	u16 addr;		/* $43xA-$43xB: HDMA indirect address */
} CPU5A22_DMA;

/* Extend CPU65816 with 5A22-specific registers */
#define CPU5A22 CPU65816
#define cpu5a22_init    cpu65816_init
#define cpu5a22_reset  cpu65816_reset
#define cpu5a22_step   cpu65816_step
#define cpu5a22_irq    cpu65816_irq
#define cpu5a22_nmi    cpu65816_nmi
#define cpu5a22_run    cpu65816_run
#define cpu5a22_dump   cpu65816_dump

#endif /* RICOH5A22 */

#if defined(__cplusplus) && !defined(__OSCAR64C__)
}
#endif

#endif /* _CPU65816_H_ */
