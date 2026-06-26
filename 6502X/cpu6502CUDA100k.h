/**
 * cpu6502CUDA100k.h - NUMCPUS parallel MOS 6502 instances on CUDA
 *
 * Instruction-level simulator (NOT transistor-level).
 * Each GPU thread simulates one independent 6502.
 * NMOS 6502 with illegal opcodes.
 *
 * RAM layout (interleaved for coalesced GPU access):
 *   - Each instance has its OWN address space (RAM_SIZE bytes)
 *   - Default: 100K × 64KB = 6.4 GB (27% of RTX 4090 24GB)
 *   - MAX mode: 11M × 2KB = ~21 GB (90% of RTX 4090 24GB)
 *   - Layout: ram[(addr & RAM_MASK) * n + instance]
 *
 * Build modes:
 *   Default:     -DNUMCPUS=100000        100K instances × 64KB = 6.4 GB
 *   MAX:         -DMAX                   11M instances × 2KB = 21 GB
 *   Custom:      -DNUMCPUS=N -DRAM_SIZE=S
 *
 * C64 ROM map (64KB mode only, 2KB mode uses set_all for PC):
 *   - $A000-$BFFF = BASIC ROM (8KB)
 *   - $E000-$FFFF = KERNAL ROM (8KB)
 *   - Use load_rom_all() to load ROMs at these addresses
 *
 * Usage:
 *   CPU100k gpu(100000);
 *   gpu.init();
 *   gpu.load_rom_all(0xA000, basic_rom, 8192);   // C64 BASIC
 *   gpu.load_rom_all(0xE000, kernal_rom, 8192);  // C64 KERNAL
 *   gpu.reset_all();
 *   gpu.step_all(1000);                            // step all 100K instances
 *   uint16_t pc = gpu.get_PC(42);                  // get PC of instance 42
 */

#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef ULTRA
/* ULTRA mode: minimal RAM (just enough for 1 idle-JMP test at $0000), max instances.
 * Full per-instance RAM lives in on-chip STATIC shared memory (byte-strided uint8, no per-step VRAM traffic),
 * so the kernel is compute-bound (~59 SASS instr/6502-step), not bandwidth-bound (breaks the ~500 GIPS VRAM ceiling).
 * Per instance: 4 (RAM) + 8 (regs) = 12 bytes.
 * 24 GiB x 0.97 / 12 = 2,083,059,138 -> 2.083B instances (~97% VRAM, int-safe, ~737 MB left for system).
 * Override on the command line: -DNUMCPUS=... */
#define RAM_SIZE 4
#define NUMCPUS 2083000000
#elif defined(MAX)
/* MAX mode: 2KB RAM per instance, maximize instance count for RTX 4090
 * 24GB VRAM × 0.9 = 21.6GB available
 * Per instance: 2048 (RAM) + 8 (regs) = 2056 bytes
 * 21.6GB / 2056 ≈ 11.3M → 11M instances (21GB used, ~1% headroom) */
#define RAM_SIZE 2048
#define NUMCPUS 11000000
#else
#ifndef NUMCPUS
#define NUMCPUS 100000
#endif
#ifndef RAM_SIZE
#define RAM_SIZE 65536
#endif
#endif

#define RAM_MASK (RAM_SIZE - 1)

namespace cpu100k {

/* P register flags */
enum : uint8_t {
	FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08,
	FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80
};

class CPU100k {
public:
	int n;  /* number of instances (default NUMCPUS) */

	CPU100k(int count = NUMCPUS) : n(count), d_A(nullptr), d_X(nullptr), d_Y(nullptr),
		d_P(nullptr), d_SP(nullptr), d_PC(nullptr), d_halted(nullptr),
		d_ram(nullptr), d_nz(nullptr), d_rom_buf(nullptr), initialized(false) {}
	~CPU100k() { free_gpu(); }

	void init();
	void free_gpu();

	/* Load same ROM data into ALL instances at given address (GPU broadcast) */
	void load_rom_all(uint16_t addr, const uint8_t* data, size_t len);

	/* Load a full 64KB image into ALL instances (GPU broadcast) */
	void load_image_all(const uint8_t* image);

	/* Reset all instances */
	void reset_all();

	/* Step all instances N times */
	void step_all(int count);

	/* Read back state for one instance */
	uint8_t  get_A(int i);
	uint8_t  get_X(int i);
	uint8_t  get_Y(int i);
	uint8_t  get_P(int i);
	uint8_t  get_SP(int i);
	uint16_t get_PC(int i);
	uint8_t  get_halted(int i);

	/* Read RAM from one instance */
	uint8_t  ram_read(int inst, uint16_t addr);
	void     ram_read_block(int inst, uint16_t addr, uint8_t* dst, size_t len);

	/* Bulk: download all instances' registers to host */
	void download_all(uint8_t* h_A, uint8_t* h_X, uint8_t* h_Y, uint8_t* h_P,
	                  uint8_t* h_SP, uint16_t* h_PC, uint8_t* h_halted);

	/* Bulk: upload all instances' registers from host */
	void upload_all(const uint8_t* h_A, const uint8_t* h_X, const uint8_t* h_Y,
	               const uint8_t* h_P, const uint8_t* h_SP, const uint16_t* h_PC);

	/* Set all instances to same register state */
	void set_all(uint8_t a, uint8_t x, uint8_t y, uint8_t p, uint8_t sp, uint16_t pc);

private:
	/* Per-instance state on GPU (SoA layout for coalesced access) */
	uint8_t  *d_A;
	uint8_t  *d_X;
	uint8_t  *d_Y;
	uint8_t  *d_P;
	uint8_t  *d_SP;
	uint16_t *d_PC;
	uint8_t  *d_halted;

	/* Per-instance 64KB RAM (interleaved): ram[addr * n + i] */
	uint8_t  *d_ram;

	/* Constant tables on GPU */
	uint8_t  *d_nz;

	/* ROM staging buffer for broadcast */
	uint8_t  *d_rom_buf;

	bool initialized;
};

} // namespace cpu100k