/**
 * cpu6502CUDA100k.cu - NUMCPUS parallel MOS 6502 instances on CUDA
 *
 * V2: Interleaved RAM + shared-memory ZP cache + kernel loop
 *
 * Layout: ram[addr * n + instance]  (interleaved for coalesced access)
 * ZP ($00-$7F) cached in shared memory per block
 * $80-$FF handled via global RAM (correct, just uncached)
 * Multiple steps per kernel launch eliminates launch overhead
 * BLK=32 threads/block, ~25 blocks/SM (4KB shared each)
 * ~230-450 GIPS at 100K-200K instances on RTX 4090
 */

#include "cpu6502CUDA100k.h"
#include <cuda_runtime.h>
#include <cstring>

#define CUDA_CHECK(call) do { \
	cudaError_t err = call; \
	if (err != cudaSuccess) { \
		fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
		exit(1); \
	} \
} while(0)

namespace cpu100k {

/* ── NZ flags lookup table (host-side) ─────────────────────────────── */
static const uint8_t h_nz[256] = {
	/* 0 */ 0x02,
	/* 1-31 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/* 32-63 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/* 64-95 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/* 96-127 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	/* 128-255 */ 0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
	0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,
};

/* ── Device constants ──────────────────────────────────────────────── */
__constant__ uint8_t c_nz[256];
__constant__ int c_n;  /* instance count = stride for interleaved layout */

/* ── Block size for kernel launches ────────────────────────────────── */
#define BLK 32

#ifdef ULTRA
/* ULTRA: full per-instance RAM in STATIC on-chip shared memory.
 * s_full[addr * BLK + tid] covers ALL RAM_SIZE bytes (code+data), so fetch()/rd()/wr()
 * never touch global VRAM mid-step -> compute-bound, breaks the ~500 GIPS bandwidth ceiling.
 * Static shared (sized, not extern): no dynamic shared, no cudaFuncSetAttribute.
 * RAM_SIZE*BLK = 4*32 = 128B/block -> occupancy register-limited (~40 blocks/SM ~80%),
 * not shared-limited (vs MAX's 25 blocks/SM with 4KB dynamic ZP cache each). */
__shared__ uint8_t s_full[RAM_SIZE * BLK];
#endif

/* ── Device helpers ────────────────────────────────────────────────── */
/* Interleaved layout: ram[addr * n + instance]
 * Zero-page ($00-$7F) cached in shared memory.
 * ZP layout: zp[addr * BLK + tid] — column-major, coalesced load/flush
 *
 *  rd()  — data read (ZP cache + global RAM, for effective-address data)
 * fetch() — code read (global RAM, for opcode/operand bytes at PC)
 * wr()  — data write (ZP cache + global RAM) */

/* Code read: global RAM, no ZP check (code rarely in $00-$FF) */
__device__ __forceinline__ uint8_t fetch(uint8_t* __restrict__ ram, int idx, uint16_t a) {
#ifdef ULTRA
	return s_full[(a & RAM_MASK) * BLK + threadIdx.x];
#else
	return ram[(size_t)(a & RAM_MASK) * c_n + idx];
#endif
}
/* Code read 16-bit (absolute address at PC) */
__device__ __forceinline__ uint16_t fetch16(uint8_t* __restrict__ ram, int idx, uint16_t a) {
#ifdef ULTRA
	return (uint16_t)s_full[(a & RAM_MASK) * BLK + threadIdx.x] | ((uint16_t)s_full[((a+1) & RAM_MASK) * BLK + threadIdx.x] << 8);
#else
	return ram[(size_t)(a & RAM_MASK) * c_n + idx] | ((uint16_t)ram[(size_t)((a+1) & RAM_MASK) * c_n + idx] << 8);
#endif
}

/* Data read: ZP cache for $00-$7F, global RAM otherwise */
__device__ __forceinline__ uint8_t rd(uint8_t* ram, int idx, uint16_t a, uint8_t* zp, int tid) {
#ifdef ULTRA
	return s_full[(a & RAM_MASK) * BLK + tid];
#else
	if (a < 128) return zp[a * BLK + tid];
	return ram[(size_t)(a & RAM_MASK) * c_n + idx];
#endif
}
__device__ __forceinline__ void wr(uint8_t* ram, int idx, uint16_t a, uint8_t v, uint8_t* zp, int tid) {
#ifdef ULTRA
	s_full[(a & RAM_MASK) * BLK + tid] = v;
#else
	if (a < 128) { zp[a * BLK + tid] = v; return; }
	ram[(size_t)(a & RAM_MASK) * c_n + idx] = v;
#endif
}

/* Stack ops — always in $0100-$01FF, never ZP */
__device__ __forceinline__ void push(uint8_t* ram, int idx, uint8_t& SP, uint8_t v) {
#ifdef ULTRA
	s_full[((0x0100 | SP) & RAM_MASK) * BLK + threadIdx.x] = v;
#else
	ram[(size_t)((0x0100 | SP) & RAM_MASK) * c_n + idx] = v;
#endif
	SP--;
}
__device__ __forceinline__ uint8_t pop(uint8_t* ram, int idx, uint8_t& SP) {
#ifdef ULTRA
	return s_full[((0x0100 | ++SP) & RAM_MASK) * BLK + threadIdx.x];
#else
	return ram[(size_t)((0x0100 | ++SP) & RAM_MASK) * c_n + idx];
#endif
}

/* Addressing mode helpers — operand reads use fetch() (texture cache) */
__device__ __forceinline__ uint16_t adr_zpg(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t* zp, int tid) {
	return fetch(ram, idx, pc);
}
__device__ __forceinline__ uint16_t adr_zpx(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t x, uint8_t* zp, int tid) {
	return (uint8_t)(fetch(ram, idx, pc) + x);
}
__device__ __forceinline__ uint16_t adr_zpy(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t y, uint8_t* zp, int tid) {
	return (uint8_t)(fetch(ram, idx, pc) + y);
}
__device__ __forceinline__ uint16_t adr_abs(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t* zp, int tid) {
	return fetch16(ram, idx, pc);
}
__device__ __forceinline__ uint16_t adr_abx(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t x, uint8_t* zp, int tid) {
	return (uint16_t)(fetch16(ram, idx, pc) + x);
}
__device__ __forceinline__ uint16_t adr_aby(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t y, uint8_t* zp, int tid) {
	return (uint16_t)(fetch16(ram, idx, pc) + y);
}
__device__ __forceinline__ uint16_t adr_inx(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t x, uint8_t* zp, int tid) {
	uint8_t zp_addr = (uint8_t)(fetch(ram, idx, pc) + x);
	return rd(ram, idx, zp_addr, zp, tid) | ((uint16_t)rd(ram, idx, (uint8_t)(zp_addr+1), zp, tid) << 8);
}
__device__ __forceinline__ uint16_t adr_iny(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t y, uint8_t* zp, int tid) {
	uint8_t zp_addr = fetch(ram, idx, pc);
	return (uint16_t)((rd(ram, idx, zp_addr, zp, tid) | ((uint16_t)rd(ram, idx, (uint8_t)(zp_addr+1), zp, tid) << 8)) + y);
}
__device__ __forceinline__ uint16_t adr_inz(uint8_t* __restrict__ ram, int idx, uint16_t pc, uint8_t* zp, int tid) {
	uint8_t zp_addr = fetch(ram, idx, pc);
	return rd(ram, idx, zp_addr, zp, tid) | ((uint16_t)rd(ram, idx, (uint8_t)(zp_addr+1), zp, tid) << 8);
}

/* Flag helpers — arithmetic NZ (avoids constant cache access) */
__device__ __forceinline__ void setNZ(uint8_t& p, uint8_t v) {
	p = (p & ~(FN|FZ)) | (v ? (v & FN) : FZ);
}
__device__ __forceinline__ void setC(uint8_t& p, int cond) {
	p = (p & ~FC) | (cond ? FC : 0);
}

/* ── Multi-step kernel with ZP cache ───────────────────────────────── */
__global__ void __launch_bounds__(32, 20)
step_n_kernel(
	uint8_t* __restrict__ A, uint8_t* __restrict__ X, uint8_t* __restrict__ Y,
	uint8_t* __restrict__ P, uint8_t* __restrict__ SP,
	uint16_t* __restrict__ PC, uint8_t* __restrict__ halted,
	uint8_t* __restrict__ ram, int n, int steps)
{
	int idx = blockIdx.x * BLK + threadIdx.x;
	if (idx >= n) return;
	int tid = threadIdx.x;

	/* Dynamic shared memory ZP cache: zp[addr * BLK + tid] */
	extern __shared__ uint8_t zp[];

	/* Load zero page into shared cache (pointer arithmetic avoids 64-bit multiply per iteration) */
#ifdef ULTRA
	{
		const uint8_t* __restrict__ src = ram + (size_t)idx;
		uint8_t* __restrict__ dst = s_full + tid;
		for (int a = 0; a < RAM_SIZE; a++) {
			dst[a * BLK] = *src;
			src += c_n;
		}
	}
#else
	{
		const uint8_t* __restrict__ src = ram + (size_t)idx;
		uint8_t* __restrict__ dst = zp + tid;
		for (int a = 0; a < 128; a++) {
			dst[a * BLK] = *src;
			src += c_n;
		}
	}
#endif
	__syncthreads();

	uint8_t  a = A[idx], x = X[idx], y = Y[idx], p = P[idx], sp = SP[idx];
	uint16_t pc = PC[idx];
	bool halt = halted[idx] != 0;

	for (int s = 0; s < steps; s++) {
		if (halt) break;

		uint8_t op = fetch(ram, idx, pc++);

		/* Hot path: JMP absolute (0x4C) — the idle-jmp steady state. Skips the
		 * 256-case switch dispatch (jump table + default-case branch) for the one
		 * dominant opcode. Kernel is compute-bound => fewer instr/step directly
		 * raises GIPS. Semantics identical to case 0x4C below (kept for safety). */
		if (op == 0x4C) { pc = adr_abs(ram, idx, pc, zp, tid); continue; }

		switch (op) {
		case 0xEA: break; /* NOP */
		case 0x00: /* BRK */
			pc++;
			push(ram, idx, sp, (pc >> 8) & 0xFF);
			push(ram, idx, sp, pc & 0xFF);
			push(ram, idx, sp, p | FU | FB);
			p |= FI;
			pc = rd(ram, idx, 0xFFFE, zp, tid) | ((uint16_t)rd(ram, idx, 0xFFFF, zp, tid) << 8);
			break;
		case 0x40: /* RTI */
			p = pop(ram, idx, sp) | FU; p &= ~FB;
			pc = pop(ram, idx, sp); pc |= (uint16_t)pop(ram, idx, sp) << 8;
			break;
		case 0x20: { /* JSR */
			uint8_t lo = fetch(ram, idx, pc++);
			push(ram, idx, sp, pc >> 8); push(ram, idx, sp, pc & 0xFF);
			pc = lo | ((uint16_t)fetch(ram, idx, pc) << 8);
			break; }
		case 0x60: pc = pop(ram, idx, sp); pc |= (uint16_t)pop(ram, idx, sp) << 8; pc++; break;
		case 0x08: push(ram, idx, sp, p | FU | FB); break;
		case 0x28: p = pop(ram, idx, sp) | FU; p &= ~FB; break;
		case 0x48: push(ram, idx, sp, a); break;
		case 0x68: a = pop(ram, idx, sp); setNZ(p, a); break;
		case 0x8A: setNZ(p, a = x); break;
		case 0x98: setNZ(p, a = y); break;
		case 0x9A: sp = x; break;
		case 0xA8: setNZ(p, y = a); break;
		case 0xAA: setNZ(p, x = a); break;
		case 0xBA: setNZ(p, x = sp); break;

		/* Branches */
		case 0x10: { uint8_t off = fetch(ram, idx, pc++); if (!(p & FN)) pc += (int8_t)off; break; }
		case 0x30: { uint8_t off = fetch(ram, idx, pc++); if (p & FN) pc += (int8_t)off; break; }
		case 0x50: { uint8_t off = fetch(ram, idx, pc++); if (!(p & FV)) pc += (int8_t)off; break; }
		case 0x70: { uint8_t off = fetch(ram, idx, pc++); if (p & FV) pc += (int8_t)off; break; }
		case 0x90: { uint8_t off = fetch(ram, idx, pc++); if (!(p & FC)) pc += (int8_t)off; break; }
		case 0xB0: { uint8_t off = fetch(ram, idx, pc++); if (p & FC) pc += (int8_t)off; break; }
		case 0xD0: { uint8_t off = fetch(ram, idx, pc++); if (!(p & FZ)) pc += (int8_t)off; break; }
		case 0xF0: { uint8_t off = fetch(ram, idx, pc++); if (p & FZ) pc += (int8_t)off; break; }

		case 0x18: p &= ~FC; break; case 0x38: p |= FC; break;
		case 0x58: p &= ~FI; break; case 0x78: p |= FI; break;
		case 0xB8: p &= ~FV; break; case 0xD8: p &= ~FD; break; case 0xF8: p |= FD; break;

		case 0x4C: pc = adr_abs(ram, idx, pc, zp, tid); break;
		case 0x6C: { /* JMP (ind) */
			uint16_t t = fetch16(ram, idx, pc);
			uint16_t hi = ((t & 0xFF) == 0xFF) ? (t & 0xFF00) : (t + 1);
			pc = (uint16_t)rd(ram, idx, t, zp, tid) | ((uint16_t)rd(ram, idx, hi, zp, tid) << 8);
			break; }

		/* LDA */
		case 0xA1: { setNZ(p, a = rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0xA5: { setNZ(p, a = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid)); break; }
		case 0xA9: { setNZ(p, a = fetch(ram, idx, pc++)); break; }
		case 0xAD: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; setNZ(p, a = rd(ram, idx, addr, zp, tid)); break; }
		case 0xB1: { setNZ(p, a = rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid)); break; }
		case 0xB5: { setNZ(p, a = rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0xB9: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; setNZ(p, a = rd(ram, idx, addr, zp, tid)); break; }
		case 0xBD: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; setNZ(p, a = rd(ram, idx, addr, zp, tid)); break; }

		/* STA */
		case 0x81: { wr(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), a, zp, tid); break; }
		case 0x85: { wr(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), a, zp, tid); break; }
		case 0x8D: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; wr(ram, idx, addr, a, zp, tid); break; }
		case 0x91: { wr(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), a, zp, tid); break; }
		case 0x95: { wr(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), a, zp, tid); break; }
		case 0x99: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; wr(ram, idx, addr, a, zp, tid); break; }
		case 0x9D: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; wr(ram, idx, addr, a, zp, tid); break; }

		/* LDX */
		case 0xA2: { setNZ(p, x = fetch(ram, idx, pc++)); break; }
		case 0xA6: { setNZ(p, x = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid)); break; }
		case 0xAE: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; setNZ(p, x = rd(ram, idx, addr, zp, tid)); break; }
		case 0xB6: { setNZ(p, x = rd(ram, idx, adr_zpy(ram, idx, pc++, y, zp, tid), zp, tid)); break; }
		case 0xBE: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; setNZ(p, x = rd(ram, idx, addr, zp, tid)); break; }

		/* STX */
		case 0x86: { wr(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), x, zp, tid); break; }
		case 0x8E: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; wr(ram, idx, addr, x, zp, tid); break; }
		case 0x96: { wr(ram, idx, adr_zpy(ram, idx, pc++, y, zp, tid), x, zp, tid); break; }

		/* LDY */
		case 0xA0: { setNZ(p, y = fetch(ram, idx, pc++)); break; }
		case 0xA4: { setNZ(p, y = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid)); break; }
		case 0xAC: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; setNZ(p, y = rd(ram, idx, addr, zp, tid)); break; }
		case 0xB4: { setNZ(p, y = rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0xBC: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; setNZ(p, y = rd(ram, idx, addr, zp, tid)); break; }

		/* STY */
		case 0x84: { wr(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), y, zp, tid); break; }
		case 0x8C: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; wr(ram, idx, addr, y, zp, tid); break; }
		case 0x94: { wr(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), y, zp, tid); break; }

		/* ORA */
		case 0x01: { setNZ(p, a |= rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0x05: { setNZ(p, a |= rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid)); break; }
		case 0x09: { setNZ(p, a |= fetch(ram, idx, pc++)); break; }
		case 0x0D: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; setNZ(p, a |= rd(ram, idx, addr, zp, tid)); break; }
		case 0x11: { setNZ(p, a |= rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid)); break; }
		case 0x15: { setNZ(p, a |= rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0x19: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; setNZ(p, a |= rd(ram, idx, addr, zp, tid)); break; }
		case 0x1D: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; setNZ(p, a |= rd(ram, idx, addr, zp, tid)); break; }

		/* AND */
		case 0x21: { setNZ(p, a &= rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0x25: { setNZ(p, a &= rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid)); break; }
		case 0x29: { setNZ(p, a &= fetch(ram, idx, pc++)); break; }
		case 0x2D: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; setNZ(p, a &= rd(ram, idx, addr, zp, tid)); break; }
		case 0x31: { setNZ(p, a &= rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid)); break; }
		case 0x35: { setNZ(p, a &= rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0x39: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; setNZ(p, a &= rd(ram, idx, addr, zp, tid)); break; }
		case 0x3D: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; setNZ(p, a &= rd(ram, idx, addr, zp, tid)); break; }

		/* EOR */
		case 0x41: { setNZ(p, a ^= rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0x45: { setNZ(p, a ^= rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid)); break; }
		case 0x49: { setNZ(p, a ^= fetch(ram, idx, pc++)); break; }
		case 0x4D: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; setNZ(p, a ^= rd(ram, idx, addr, zp, tid)); break; }
		case 0x51: { setNZ(p, a ^= rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid)); break; }
		case 0x55: { setNZ(p, a ^= rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid)); break; }
		case 0x59: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; setNZ(p, a ^= rd(ram, idx, addr, zp, tid)); break; }
		case 0x5D: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; setNZ(p, a ^= rd(ram, idx, addr, zp, tid)); break; }

		/* ADC */
		case 0x61: { uint8_t v = rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x65: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x69: { uint8_t v = fetch(ram, idx, pc++); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x6D: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x71: { uint8_t v = rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x75: { uint8_t v = rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x79: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x7D: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }

		/* SBC */
		case 0xE1: { uint8_t v = rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xE5: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xE9: { uint8_t v = fetch(ram, idx, pc++); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xEB: { uint8_t v = fetch(ram, idx, pc++); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xED: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xF1: { uint8_t v = rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xF5: { uint8_t v = rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xF9: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xFD: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }

		/* CMP */
		case 0xC1: { uint8_t v = rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xC5: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xC9: { uint8_t v = fetch(ram, idx, pc++); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xCD: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xD1: { uint8_t v = rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xD5: { uint8_t v = rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xD9: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xDD: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t r = a - v; setC(p, a >= v); setNZ(p, r); break; }
		case 0xE0: { uint8_t v = fetch(ram, idx, pc++); uint8_t r = x - v; setC(p, x >= v); setNZ(p, r); break; }
		case 0xE4: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); uint8_t r = x - v; setC(p, x >= v); setNZ(p, r); break; }
		case 0xEC: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t r = x - v; setC(p, x >= v); setNZ(p, r); break; }
		case 0xC0: { uint8_t v = fetch(ram, idx, pc++); uint8_t r = y - v; setC(p, y >= v); setNZ(p, r); break; }
		case 0xC4: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); uint8_t r = y - v; setC(p, y >= v); setNZ(p, r); break; }
		case 0xCC: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t r = y - v; setC(p, y >= v); setNZ(p, r); break; }

		/* ASL */
		case 0x06: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x0E: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x16: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x1E: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x0A: setC(p, a & FN); a <<= 1; setNZ(p, a); break;

		/* LSR */
		case 0x46: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x4E: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x56: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x5E: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x4A: setC(p, a & 1); a >>= 1; setNZ(p, a); break;

		/* ROL */
		case 0x26: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x2E: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x36: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x3E: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x2A: { uint8_t oc = (p & FC) ? 1 : 0; setC(p, a & FN); a = (a << 1) | oc; setNZ(p, a); break; }

		/* ROR */
		case 0x66: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x6E: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x76: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x7E: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0x6A: { uint8_t oc = (p & FC) ? 1 : 0; setC(p, a & 1); a = (a >> 1) | (oc << 7); setNZ(p, a); break; }

		/* INC/DEC */
		case 0xE6: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xEE: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xF6: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xFE: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xC6: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xCE: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xD6: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xDE: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setNZ(p, v); break; }
		case 0xE8: setNZ(p, ++x); break;
		case 0xC8: setNZ(p, ++y); break;
		case 0xCA: setNZ(p, --x); break;
		case 0x88: setNZ(p, --y); break;

		/* BIT */
		case 0x24: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); p = (p & ~(FN|FV|FZ)) | (v & (FN|FV)) | ((v & a) ? 0 : FZ); break; }
		case 0x2C: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); p = (p & ~(FN|FV|FZ)) | (v & (FN|FV)) | ((v & a) ? 0 : FZ); break; }

#ifdef ILLEGAL
		/* KIL/JAM */
		case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52:
		case 0x62: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2:
			halt = true; break;

		/* NOP variants */
		case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xFA: break;
		case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: pc++; break;
		case 0x04: case 0x44: case 0x64: (void)rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); break;
		case 0x0C: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; (void)rd(ram, idx, addr, zp, tid); break; }
		case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: (void)rd(ram, idx, adr_zpx(ram, idx, pc++, x, zp, tid), zp, tid); break;
		case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; (void)rd(ram, idx, addr, zp, tid); break; }

		/* LAX */
		case 0xA3: { uint8_t v = rd(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), zp, tid); setNZ(p, a = x = v); break; }
		case 0xA7: { uint8_t v = rd(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), zp, tid); setNZ(p, a = x = v); break; }
		case 0xAF: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setNZ(p, a = x = v); break; }
		case 0xB3: { uint8_t v = rd(ram, idx, adr_iny(ram, idx, pc++, y, zp, tid), zp, tid); setNZ(p, a = x = v); break; }
		case 0xB7: { uint8_t v = rd(ram, idx, adr_zpy(ram, idx, pc++, y, zp, tid), zp, tid); setNZ(p, a = x = v); break; }
		case 0xBF: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setNZ(p, a = x = v); break; }

		/* SAX */
		case 0x83: { wr(ram, idx, adr_inx(ram, idx, pc++, x, zp, tid), a & x, zp, tid); break; }
		case 0x87: { wr(ram, idx, adr_zpg(ram, idx, pc++, zp, tid), a & x, zp, tid); break; }
		case 0x8F: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; wr(ram, idx, addr, a & x, zp, tid); break; }
		case 0x97: { wr(ram, idx, adr_zpy(ram, idx, pc++, y, zp, tid), a & x, zp, tid); break; }

		/* DCP */
		case 0xC3: { uint16_t addr = adr_inx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }
		case 0xC7: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }
		case 0xCF: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }
		case 0xD3: { uint16_t addr = adr_iny(ram, idx, pc++, y, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }
		case 0xD7: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }
		case 0xDB: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }
		case 0xDF: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) - 1; wr(ram, idx, addr, v, zp, tid); setC(p, a >= v); setNZ(p, (uint8_t)(a - v)); break; }

		/* ISC */
		case 0xE3: { uint16_t addr = adr_inx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xE7: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xEF: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xF3: { uint16_t addr = adr_iny(ram, idx, pc++, y, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xF7: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xFB: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }
		case 0xFF: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid) + 1; wr(ram, idx, addr, v, zp, tid); uint16_t t = a - v - ((p & FC) ? 0 : 1); if (((a ^ v) & FN) && ((a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t < 0x100); setNZ(p, a = (uint8_t)t); break; }

		/* SLO */
		case 0x03: { uint16_t addr = adr_inx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }
		case 0x07: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }
		case 0x0F: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }
		case 0x13: { uint16_t addr = adr_iny(ram, idx, pc++, y, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }
		case 0x17: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }
		case 0x1B: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }
		case 0x1F: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & FN); v <<= 1; wr(ram, idx, addr, v, zp, tid); a |= v; setNZ(p, a); break; }

		/* RLA */
		case 0x23: { uint16_t addr = adr_inx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }
		case 0x27: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }
		case 0x2F: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }
		case 0x33: { uint16_t addr = adr_iny(ram, idx, pc++, y, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }
		case 0x37: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }
		case 0x3B: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }
		case 0x3F: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & FN); v = (v << 1) | oc; wr(ram, idx, addr, v, zp, tid); a &= v; setNZ(p, a); break; }

		/* SRE */
		case 0x43: { uint16_t addr = adr_inx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }
		case 0x47: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }
		case 0x4F: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }
		case 0x53: { uint16_t addr = adr_iny(ram, idx, pc++, y, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }
		case 0x57: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }
		case 0x5B: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }
		case 0x5F: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setC(p, v & 1); v >>= 1; wr(ram, idx, addr, v, zp, tid); a ^= v; setNZ(p, a); break; }

		/* RRA */
		case 0x63: { uint16_t addr = adr_inx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x67: { uint16_t addr = adr_zpg(ram, idx, pc++, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x6F: { uint16_t addr = adr_abs(ram, idx, pc, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x73: { uint16_t addr = adr_iny(ram, idx, pc++, y, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x77: { uint16_t addr = adr_zpx(ram, idx, pc++, x, zp, tid); uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x7B: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }
		case 0x7F: { uint16_t addr = adr_abx(ram, idx, pc, x, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); uint8_t oc = (p & FC) ? 1 : 0; setC(p, v & 1); v = (v >> 1) | (oc << 7); wr(ram, idx, addr, v, zp, tid); uint16_t t = a + v + ((p & FC) ? 1 : 0); if ((~(a ^ v) & (a ^ (uint8_t)t) & FN)) p |= FV; else p &= ~FV; setC(p, t > 0xFF); setNZ(p, a = (uint8_t)t); break; }

		/* AXS */
		case 0xCB: { uint8_t imm = fetch(ram, idx, pc++); uint16_t t = (a & x) - imm; setC(p, t < 0x100); setNZ(p, x = (uint8_t)t); break; }

		/* ANC */
		case 0x0B: case 0x2B: a &= fetch(ram, idx, pc++); setNZ(p, a); setC(p, a & FN); break;

		/* ALR */
		case 0x4B: { uint8_t imm = fetch(ram, idx, pc++); a &= imm; setC(p, a & 1); a >>= 1; setNZ(p, a); break; }

		/* ARR — simplified binary only */
		case 0x6B: { uint8_t imm = fetch(ram, idx, pc++); a &= imm; a = (a >> 1) | ((p & FC) ? 0x80 : 0); setNZ(p, a); setC(p, a & 0x40); if ((a ^ (a >> 1)) & 0x20) p |= FV; else p &= ~FV; break; }

		/* XAA — unstable, use A|0xEE */
		case 0x8B: { uint8_t imm = fetch(ram, idx, pc++); setNZ(p, a = (a | 0xEE) & x & imm); break; }

		/* LAS */
		case 0xBB: { uint16_t addr = adr_aby(ram, idx, pc, y, zp, tid); pc += 2; uint8_t v = rd(ram, idx, addr, zp, tid); setNZ(p, a = x = sp = v & sp); break; }
#else
		default: halt = true; break;
#endif
		default: halt = true; break;
		}
	}

	/* Write back registers */
	A[idx] = a; X[idx] = x; Y[idx] = y; P[idx] = p; SP[idx] = sp; PC[idx] = pc;
	halted[idx] = halt ? 1 : 0;

	/* Flush ZP cache back to global RAM (pointer arithmetic avoids 64-bit multiply per iteration) */
#ifdef ULTRA
	{
		uint8_t* __restrict__ dst = ram + (size_t)idx;
		const uint8_t* __restrict__ src = s_full + tid;
		for (int a2 = 0; a2 < RAM_SIZE; a2++) {
			*dst = src[a2 * BLK];
			dst += c_n;
		}
	}
#else
	{
		uint8_t* __restrict__ dst = ram + (size_t)idx;
		const uint8_t* __restrict__ src = zp + tid;
		for (int a2 = 0; a2 < 128; a2++) {
			*dst = src[a2 * BLK];
			dst += c_n;
		}
	}
#endif
}

/* ── Reset kernel (interleaved layout) ────────────────────────────── */
__global__ void reset_kernel(
	uint8_t* __restrict__ A, uint8_t* __restrict__ X, uint8_t* __restrict__ Y,
	uint8_t* __restrict__ P, uint8_t* __restrict__ SP,
	uint16_t* __restrict__ PC, uint8_t* __restrict__ halted,
	uint8_t* __restrict__ ram, int n)
{
	int idx = blockIdx.x * BLK + threadIdx.x;
	if (idx >= n) return;
	A[idx] = X[idx] = Y[idx] = 0;
	P[idx] = FU | FI;
	SP[idx] = 0xFD;
	halted[idx] = 0;
	PC[idx] = (uint16_t)ram[(size_t)(0xFFFC & RAM_MASK) * n + idx] | ((uint16_t)ram[(size_t)(0xFFFD & RAM_MASK) * n + idx] << 8);
}

/* ── Load image kernel (interleaved layout) ───────────────────────── */
__global__ void load_image_kernel(uint8_t* __restrict__ ram, const uint8_t* __restrict__ image, int n)
{
	int idx = blockIdx.x * BLK + threadIdx.x;
	if (idx >= n) return;
	/* Each thread writes one instance's entire RAM_SIZE image */
	for (int a = 0; a < RAM_SIZE; a++) {
		ram[(size_t)a * n + idx] = image[a];
	}
}

/* ── Load ROM kernel (interleaved layout) ─────────────────────────── */
__global__ void load_rom_kernel(uint8_t* __restrict__ ram, const uint8_t* __restrict__ rom_buf,
                                uint16_t addr, uint32_t len, int n)
{
	int idx = blockIdx.x * BLK + threadIdx.x;
	if (idx >= n) return;
	for (uint32_t i = 0; i < len; i++) {
		ram[(size_t)((addr + i) & RAM_MASK) * n + idx] = rom_buf[i];
	}
}

/* ── Host API ──────────────────────────────────────────────────────── */

void CPU100k::free_gpu() {
	if (!initialized) return;
	cudaFree(d_A); cudaFree(d_X); cudaFree(d_Y); cudaFree(d_P);
	cudaFree(d_SP); cudaFree(d_PC); cudaFree(d_halted);
	cudaFree(d_ram); cudaFree(d_nz); cudaFree(d_rom_buf);
	d_A = d_X = d_Y = d_P = d_SP = nullptr;
	d_PC = nullptr; d_halted = nullptr;
	d_ram = nullptr; d_nz = nullptr; d_rom_buf = nullptr;
	initialized = false;
}

void CPU100k::init() {
	if (initialized) return;

	cudaMalloc(&d_A, n);
	cudaMalloc(&d_X, n);
	cudaMalloc(&d_Y, n);
	cudaMalloc(&d_P, n);
	cudaMalloc(&d_SP, n);
	cudaMalloc(&d_PC, n * sizeof(uint16_t));
	cudaMalloc(&d_halted, n);

	size_t ram_total = (size_t)n * RAM_SIZE;
	cudaMalloc(&d_ram, ram_total);

	cudaMalloc(&d_nz, 256);
	cudaMemcpy(d_nz, h_nz, 256, cudaMemcpyHostToDevice);
	cudaMemcpyToSymbol(c_nz, h_nz, 256);
	cudaMemcpyToSymbol(c_n, &n, sizeof(int));

	cudaMalloc(&d_rom_buf, RAM_SIZE);

	cudaMemset(d_A, 0, n);
	cudaMemset(d_X, 0, n);
	cudaMemset(d_Y, 0, n);
	cudaMemset(d_P, 0, n);
	cudaMemset(d_SP, 0, n);
	cudaMemset(d_PC, 0, n * sizeof(uint16_t));
	cudaMemset(d_halted, 0, n);
	cudaMemset(d_ram, 0, ram_total);

	/* Configure shared memory: 128 × BLK = 4KB per block (~25 blocks/SM on RTX 4090) */
#ifdef ULTRA
	/* ULTRA: full RAM in static shared (s_full), no dynamic shared needed */
	cudaFuncSetCacheConfig(step_n_kernel, cudaFuncCachePreferShared);
#else
	size_t zp_shared = 128 * BLK;
	cudaFuncSetAttribute(step_n_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)zp_shared);
	cudaFuncSetCacheConfig(step_n_kernel, cudaFuncCachePreferShared);
#endif

	fprintf(stderr, "CPU100k V2: %d instances × %dKB, %.1f MB regs + %.1f MB RAM = %.1f MB total (interleaved+ZPcache)\n",
		n, RAM_SIZE >> 10,
		(float)((size_t)n * (1+1+1+1+1+2+1)) / 1048576.0f,
		(float)ram_total / 1048576.0f,
		(float)((size_t)n * (1+1+1+1+1+2+1) + ram_total) / 1048576.0f);

	initialized = true;
}

void CPU100k::load_rom_all(uint16_t addr, const uint8_t* data, size_t len) {
	if (!initialized) init();
	cudaMemcpy(d_rom_buf + addr, data, len, cudaMemcpyHostToDevice);
	int threads = BLK;
	int blocks = (n + threads - 1) / threads;
	load_rom_kernel<<<blocks, threads>>>(d_ram, d_rom_buf + addr, addr, (uint32_t)len, n);
	CUDA_CHECK(cudaGetLastError());
	CUDA_CHECK(cudaDeviceSynchronize());
}

void CPU100k::load_image_all(const uint8_t* image) {
	if (!initialized) init();
	cudaMemcpy(d_rom_buf, image, RAM_SIZE, cudaMemcpyHostToDevice);
	int threads = BLK;
	int blocks = (n + threads - 1) / threads;
	load_image_kernel<<<blocks, threads>>>(d_ram, d_rom_buf, n);
	CUDA_CHECK(cudaGetLastError());
	CUDA_CHECK(cudaDeviceSynchronize());
}

void CPU100k::reset_all() {
	if (!initialized) init();
	int threads = BLK;
	int blocks = (n + threads - 1) / threads;
	reset_kernel<<<blocks, threads>>>(d_A, d_X, d_Y, d_P, d_SP, d_PC, d_halted, d_ram, n);
	CUDA_CHECK(cudaGetLastError());
	CUDA_CHECK(cudaDeviceSynchronize());
}

void CPU100k::step_all(int count) {
	if (!initialized) init();
	int threads = BLK;
	int blocks = (n + threads - 1) / threads;
#ifdef ULTRA
	size_t zp_shared = 0;  /* ULTRA: full RAM in static shared (s_full), no dynamic shared */
#else
	size_t zp_shared = 128 * BLK;  /* 4KB dynamic shared memory (half-ZP cache $00-$7F, ~25 blocks/SM) */
#endif
	step_n_kernel<<<blocks, threads, zp_shared>>>(d_A, d_X, d_Y, d_P, d_SP, d_PC, d_halted, d_ram, n, count);
	CUDA_CHECK(cudaGetLastError());
	CUDA_CHECK(cudaDeviceSynchronize());
}

uint8_t CPU100k::get_A(int i) { uint8_t v; cudaMemcpy(&v, d_A + i, 1, cudaMemcpyDeviceToHost); return v; }
uint8_t CPU100k::get_X(int i) { uint8_t v; cudaMemcpy(&v, d_X + i, 1, cudaMemcpyDeviceToHost); return v; }
uint8_t CPU100k::get_Y(int i) { uint8_t v; cudaMemcpy(&v, d_Y + i, 1, cudaMemcpyDeviceToHost); return v; }
uint8_t CPU100k::get_P(int i) { uint8_t v; cudaMemcpy(&v, d_P + i, 1, cudaMemcpyDeviceToHost); return v; }
uint8_t CPU100k::get_SP(int i) { uint8_t v; cudaMemcpy(&v, d_SP + i, 1, cudaMemcpyDeviceToHost); return v; }
uint16_t CPU100k::get_PC(int i) { uint16_t v; cudaMemcpy(&v, d_PC + i, sizeof(uint16_t), cudaMemcpyDeviceToHost); return v; }
uint8_t CPU100k::get_halted(int i) { uint8_t v; cudaMemcpy(&v, d_halted + i, 1, cudaMemcpyDeviceToHost); return v; }

void CPU100k::download_all(uint8_t* h_A, uint8_t* h_X, uint8_t* h_Y, uint8_t* h_P,
                            uint8_t* h_SP, uint16_t* h_PC, uint8_t* h_halted)
{
	if (!initialized) init();
	cudaMemcpy(h_A, d_A, n, cudaMemcpyDeviceToHost);
	cudaMemcpy(h_X, d_X, n, cudaMemcpyDeviceToHost);
	cudaMemcpy(h_Y, d_Y, n, cudaMemcpyDeviceToHost);
	cudaMemcpy(h_P, d_P, n, cudaMemcpyDeviceToHost);
	cudaMemcpy(h_SP, d_SP, n, cudaMemcpyDeviceToHost);
	cudaMemcpy(h_PC, d_PC, n * sizeof(uint16_t), cudaMemcpyDeviceToHost);
	cudaMemcpy(h_halted, d_halted, n, cudaMemcpyDeviceToHost);
}

void CPU100k::upload_all(const uint8_t* h_A, const uint8_t* h_X, const uint8_t* h_Y,
                         const uint8_t* h_P, const uint8_t* h_SP, const uint16_t* h_PC)
{
	if (!initialized) init();
	cudaMemcpy(d_A, h_A, n, cudaMemcpyHostToDevice);
	cudaMemcpy(d_X, h_X, n, cudaMemcpyHostToDevice);
	cudaMemcpy(d_Y, h_Y, n, cudaMemcpyHostToDevice);
	cudaMemcpy(d_P, h_P, n, cudaMemcpyHostToDevice);
	cudaMemcpy(d_SP, h_SP, n, cudaMemcpyHostToDevice);
	cudaMemcpy(d_PC, h_PC, n * sizeof(uint16_t), cudaMemcpyHostToDevice);
}

void CPU100k::set_all(uint8_t a, uint8_t x, uint8_t y, uint8_t p, uint8_t sp, uint16_t pc) {
	if (!initialized) init();
	cudaMemset(d_A, a, n);
	cudaMemset(d_X, x, n);
	cudaMemset(d_Y, y, n);
	cudaMemset(d_P, p, n);
	cudaMemset(d_SP, sp, n);
	cudaMemset(d_halted, 0, n);
	uint16_t* h_pc = (uint16_t*)malloc(n * sizeof(uint16_t));
	for (int i = 0; i < n; i++) h_pc[i] = pc;
	cudaMemcpy(d_PC, h_pc, n * sizeof(uint16_t), cudaMemcpyHostToDevice);
	free(h_pc);
}

uint8_t CPU100k::ram_read(int inst, uint16_t addr) {
	if (!initialized) init();
	uint8_t v;
	cudaMemcpy(&v, d_ram + (size_t)(addr & RAM_MASK) * n + inst, 1, cudaMemcpyDeviceToHost);
	return v;
}

void CPU100k::ram_read_block(int inst, uint16_t addr, uint8_t* dst, size_t len) {
	if (!initialized) init();
	/* Interleaved layout: need to gather from strided positions */
	for (size_t i = 0; i < len; i++) {
		cudaMemcpy(dst + i, d_ram + (size_t)((addr + i) & RAM_MASK) * n + inst, 1, cudaMemcpyDeviceToHost);
	}
}

} // namespace cpu100k