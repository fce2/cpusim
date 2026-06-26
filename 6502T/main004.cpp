// transistor-level 6502 test ZIP generator (Tom Harte style)

#include "cpu6502T.h" // use the "silicon cpu" for perfect test data
#include "compress.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define ZIP_FILE "65x02-perfect.zip"
#define ZIP_PATH "65x02-main/6502/v1/%02x.json"

#define RND(s) ((u8)((s) = (s) * 6364136223846793005ULL + 1442695040888963407ULL, (u8)((s) >> 33)))

enum { AM_IMP,AM_IMM,AM_ZPG,AM_ZPX,AM_ZPY,AM_IZX,AM_IZY,AM_ABS,AM_ABX,AM_ABY,AM_REL,AM_IND };
enum : u8 {
	OP_ADC,OP_AND,OP_ASL,OP_BCC,OP_BCS,OP_BEQ,OP_BIT,OP_BMI,OP_BNE,OP_BPL,OP_BRK,OP_BVC,OP_BVS,
	OP_CLC,OP_CLD,OP_CLI,OP_CLV,OP_CMP,OP_CPX,OP_CPY,OP_DEC,OP_DEX,OP_DEY,OP_EOR,OP_INC,OP_INX,
	OP_INY,OP_JMP,OP_JSR,OP_LDA,OP_LDX,OP_LDY,OP_LSR,OP_NOP,OP_ORA,OP_PHA,OP_PHP,OP_PLA,OP_PLP,
	OP_ROL,OP_ROR,OP_RTI,OP_RTS,OP_SBC,OP_SEC,OP_SED,OP_SEI,OP_STA,OP_STX,OP_STY,OP_TAX,OP_TAY,
	OP_TSX,OP_TXA,OP_TXS,OP_TYA,OP_KIL,OP_SLO,OP_RLA,OP_SRE,OP_RRA,OP_SAX,OP_LAX,OP_DCP,OP_ISB,
	OP_ANC,OP_ALR,OP_ARR,OP_AXS,OP_SHS,OP_SHX,OP_SHY,OP_LAS,OP_AXA
};

struct OP_INFO { u8 op, mode, len; };
static constexpr OP_INFO ops[256] = {
	{OP_BRK,AM_IMP,1},{OP_ORA,AM_IZX,2},{OP_KIL,AM_IMP,1},{OP_SLO,AM_IZX,2},{OP_NOP,AM_ZPG,2},{OP_ORA,AM_ZPG,2},{OP_ASL,AM_ZPG,2},{OP_SLO,AM_ZPG,2},{OP_PHP,AM_IMP,1},{OP_ORA,AM_IMM,2},{OP_ASL,AM_IMP,1},{OP_ANC,AM_IMM,2},{OP_NOP,AM_ABS,3},{OP_ORA,AM_ABS,3},{OP_ASL,AM_ABS,3},{OP_SLO,AM_ABS,3},
	{OP_BPL,AM_REL,2},{OP_ORA,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_SLO,AM_IZY,2},{OP_NOP,AM_ZPX,2},{OP_ORA,AM_ZPX,2},{OP_ASL,AM_ZPX,2},{OP_SLO,AM_ZPX,2},{OP_CLC,AM_IMP,1},{OP_ORA,AM_ABY,3},{OP_NOP,AM_IMP,1},{OP_SLO,AM_ABY,3},{OP_NOP,AM_ABX,3},{OP_ORA,AM_ABX,3},{OP_ASL,AM_ABX,3},{OP_SLO,AM_ABX,3},
	{OP_JSR,AM_ABS,3},{OP_AND,AM_IZX,2},{OP_KIL,AM_IMP,1},{OP_RLA,AM_IZX,2},{OP_BIT,AM_ZPG,2},{OP_AND,AM_ZPG,2},{OP_ROL,AM_ZPG,2},{OP_RLA,AM_ZPG,2},{OP_PLP,AM_IMP,1},{OP_AND,AM_IMM,2},{OP_ROL,AM_IMP,1},{OP_ANC,AM_IMM,2},{OP_BIT,AM_ABS,3},{OP_AND,AM_ABS,3},{OP_ROL,AM_ABS,3},{OP_RLA,AM_ABS,3},
	{OP_BMI,AM_REL,2},{OP_AND,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_RLA,AM_IZY,2},{OP_NOP,AM_ZPX,2},{OP_AND,AM_ZPX,2},{OP_ROL,AM_ZPX,2},{OP_RLA,AM_ZPX,2},{OP_SEC,AM_IMP,1},{OP_AND,AM_ABY,3},{OP_NOP,AM_IMP,1},{OP_RLA,AM_ABY,3},{OP_NOP,AM_ABX,3},{OP_AND,AM_ABX,3},{OP_ROL,AM_ABX,3},{OP_RLA,AM_ABX,3},
	{OP_RTI,AM_IMP,1},{OP_EOR,AM_IZX,2},{OP_KIL,AM_IMP,1},{OP_SRE,AM_IZX,2},{OP_NOP,AM_ZPG,2},{OP_EOR,AM_ZPG,2},{OP_LSR,AM_ZPG,2},{OP_SRE,AM_ZPG,2},{OP_PHA,AM_IMP,1},{OP_EOR,AM_IMM,2},{OP_LSR,AM_IMP,1},{OP_ALR,AM_IMM,2},{OP_JMP,AM_ABS,3},{OP_EOR,AM_ABS,3},{OP_LSR,AM_ABS,3},{OP_SRE,AM_ABS,3},
	{OP_BVC,AM_REL,2},{OP_EOR,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_SRE,AM_IZY,2},{OP_NOP,AM_ZPX,2},{OP_EOR,AM_ZPX,2},{OP_LSR,AM_ZPX,2},{OP_SRE,AM_ZPX,2},{OP_CLI,AM_IMP,1},{OP_EOR,AM_ABY,3},{OP_NOP,AM_IMP,1},{OP_SRE,AM_ABY,3},{OP_NOP,AM_ABX,3},{OP_EOR,AM_ABX,3},{OP_LSR,AM_ABX,3},{OP_SRE,AM_ABX,3},
	{OP_RTS,AM_IMP,1},{OP_ADC,AM_IZX,2},{OP_KIL,AM_IMP,1},{OP_RRA,AM_IZX,2},{OP_NOP,AM_ZPG,2},{OP_ADC,AM_ZPG,2},{OP_ROR,AM_ZPG,2},{OP_RRA,AM_ZPG,2},{OP_PHA,AM_IMP,1},{OP_ADC,AM_IMM,2},{OP_ROR,AM_IMP,1},{OP_ARR,AM_IMM,2},{OP_JMP,AM_IND,3},{OP_ADC,AM_ABS,3},{OP_ROR,AM_ABS,3},{OP_RRA,AM_ABS,3},
	{OP_BVS,AM_REL,2},{OP_ADC,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_RRA,AM_IZY,2},{OP_NOP,AM_ZPX,2},{OP_ADC,AM_ZPX,2},{OP_ROR,AM_ZPX,2},{OP_RRA,AM_ZPX,2},{OP_SEI,AM_IMP,1},{OP_ADC,AM_ABY,3},{OP_NOP,AM_IMP,1},{OP_RRA,AM_ABY,3},{OP_NOP,AM_ABX,3},{OP_ADC,AM_ABX,3},{OP_ROR,AM_ABX,3},{OP_RRA,AM_ABX,3},
	{OP_NOP,AM_IMM,2},{OP_STA,AM_IZX,2},{OP_NOP,AM_IMM,2},{OP_SAX,AM_IZX,2},{OP_STY,AM_ZPG,2},{OP_STA,AM_ZPG,2},{OP_STX,AM_ZPG,2},{OP_SAX,AM_ZPG,2},{OP_DEY,AM_IMP,1},{OP_NOP,AM_IMM,2},{OP_TXA,AM_IMP,1},{OP_AXA,AM_IMM,2},{OP_STY,AM_ABS,3},{OP_STA,AM_ABS,3},{OP_STX,AM_ABS,3},{OP_SAX,AM_ABS,3},
	{OP_BCC,AM_REL,2},{OP_STA,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_AXA,AM_IZY,2},{OP_STY,AM_ZPX,2},{OP_STA,AM_ZPX,2},{OP_STX,AM_ZPY,2},{OP_SAX,AM_ZPY,2},{OP_TYA,AM_IMP,1},{OP_STA,AM_ABY,3},{OP_TXS,AM_IMP,1},{OP_SHS,AM_ABY,3},{OP_SHY,AM_ABX,3},{OP_STA,AM_ABX,3},{OP_SHX,AM_ABY,3},{OP_AXA,AM_ABY,3},
	{OP_LDY,AM_IMM,2},{OP_LDA,AM_IZX,2},{OP_LDX,AM_IMM,2},{OP_LAX,AM_IZX,2},{OP_LDY,AM_ZPG,2},{OP_LDA,AM_ZPG,2},{OP_LDX,AM_ZPG,2},{OP_LAX,AM_ZPG,2},{OP_TAY,AM_IMP,1},{OP_LDA,AM_IMM,2},{OP_TAX,AM_IMP,1},{OP_LAX,AM_IMM,2},{OP_LDY,AM_ABS,3},{OP_LDA,AM_ABS,3},{OP_LDX,AM_ABS,3},{OP_LAX,AM_ABS,3},
	{OP_BCS,AM_REL,2},{OP_LDA,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_LAX,AM_IZY,2},{OP_LDY,AM_ZPX,2},{OP_LDA,AM_ZPX,2},{OP_LDX,AM_ZPY,2},{OP_LAX,AM_ZPY,2},{OP_CLV,AM_IMP,1},{OP_LDA,AM_ABY,3},{OP_TSX,AM_IMP,1},{OP_LAS,AM_ABY,3},{OP_LDY,AM_ABX,3},{OP_LDA,AM_ABX,3},{OP_LDX,AM_ABY,3},{OP_LAX,AM_ABY,3},
	{OP_CPY,AM_IMM,2},{OP_CMP,AM_IZX,2},{OP_NOP,AM_IMM,2},{OP_DCP,AM_IZX,2},{OP_CPY,AM_ZPG,2},{OP_CMP,AM_ZPG,2},{OP_DEC,AM_ZPG,2},{OP_DCP,AM_ZPG,2},{OP_INY,AM_IMP,1},{OP_CMP,AM_IMM,2},{OP_DEX,AM_IMP,1},{OP_AXS,AM_IMM,2},{OP_CPY,AM_ABS,3},{OP_CMP,AM_ABS,3},{OP_DEC,AM_ABS,3},{OP_DCP,AM_ABS,3},
	{OP_BNE,AM_REL,2},{OP_CMP,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_DCP,AM_IZY,2},{OP_NOP,AM_ZPX,2},{OP_CMP,AM_ZPX,2},{OP_DEC,AM_ZPX,2},{OP_DCP,AM_ZPX,2},{OP_CLD,AM_IMP,1},{OP_CMP,AM_ABY,3},{OP_NOP,AM_IMP,1},{OP_DCP,AM_ABY,3},{OP_NOP,AM_ABX,3},{OP_CMP,AM_ABX,3},{OP_DEC,AM_ABX,3},{OP_DCP,AM_ABX,3},
	{OP_CPX,AM_IMM,2},{OP_SBC,AM_IZX,2},{OP_NOP,AM_IMM,2},{OP_ISB,AM_IZX,2},{OP_CPX,AM_ZPG,2},{OP_SBC,AM_ZPG,2},{OP_INC,AM_ZPG,2},{OP_ISB,AM_ZPG,2},{OP_INX,AM_IMP,1},{OP_SBC,AM_IMM,2},{OP_NOP,AM_IMP,1},{OP_SBC,AM_IMM,2},{OP_CPX,AM_ABS,3},{OP_SBC,AM_ABS,3},{OP_INC,AM_ABS,3},{OP_ISB,AM_ABS,3},
	{OP_BEQ,AM_REL,2},{OP_SBC,AM_IZY,2},{OP_KIL,AM_IMP,1},{OP_ISB,AM_IZY,2},{OP_NOP,AM_ZPX,2},{OP_SBC,AM_ZPX,2},{OP_INC,AM_ZPX,2},{OP_ISB,AM_ZPX,2},{OP_SED,AM_IMP,1},{OP_SBC,AM_ABY,3},{OP_NOP,AM_IMP,1},{OP_ISB,AM_ABY,3},{OP_NOP,AM_ABX,3},{OP_SBC,AM_ABX,3},{OP_INC,AM_ABX,3},{OP_ISB,AM_ABX,3}
};

static constexpr u16 TEST_ADDR = 0x0300;
using RAM = std::array<u8, 65536>;

template<typename... Args>
static void fmt(std::string& s, const char* f, Args... a) { char buf[1024]; s.append(buf, std::snprintf(buf, sizeof(buf), f, a...)); }

static std::string json_ram(const RAM& r) {
	std::string s;
	s.reserve(65536);
	bool first = true;
	for (int i = 0; i < 65536; i++)
		if (r[i]) {
			if (!first) s += ',';
			fmt(s, "[%d,%d]", i, r[i]);
			first = false;
		}
	return s;
}

static std::string json_ram_diff(const RAM& r0, const RAM& r1) {
	std::string s;
	s.reserve(65536);
	bool first = true;
	for (int i = 0; i < 65536; i++)
		if (r0[i] != r1[i]) {
			if (!first) s += ',';
			fmt(s, "[%d,%d]", i, r1[i]);
			first = false;
		}
	return s;
}

static std::string json_cycles(const CPU6502T_Cycle *cyc, int n) {
	std::string s;
	s.reserve(n * 32);
	for (int i = 0; i < n; i++) {
		if (i) s += ',';
		fmt(s, "[%d,%d,\"%s\"]", cyc[i].addr, cyc[i].data, cyc[i].write ? "write" : "read");
	}
	return s;
}

static std::string gen_opcode(RAM& ram, CPU6502T& cpu, int opc, int count, u64 seed) {
	auto info = ops[opc];
	bool is_kil = (info.op == OP_KIL);
	std::string buf;
	buf.reserve(count * 2048 + 16384);
	buf = "[\n";
	for (int t = 0; t < count; t++) {
		u64 s = seed + (u64)t * 2654435761ULL;
		u8 a  = RND(s);
		u8 x  = RND(s);
		u8 y  = RND(s);
		u8 sp = RND(s);
		if (sp < 3) sp += 3;
		u8 p   = (RND(s) | 0x20) & ~0x10;
		u8 op1 = RND(s);
		u8 op2 = RND(s);
		ram.fill(0);
		ram[0xFFFC] = 0x00; ram[0xFFFD] = 0x02;
		ram[0xFFFE] = 0x00; ram[0xFFFF] = 0x02;
		ram[TEST_ADDR] = (u8)opc;
		if (info.len >= 2) ram[TEST_ADDR+1] = op1;
		if (info.len >= 3) ram[TEST_ADDR+2] = op2;
		for (int i = info.len; i < info.len + 3; i++) ram[TEST_ADDR + i] = 0xEA;
		if (!is_kil) {
			u8 zp; u16 addr;
			switch (info.mode) {
			case AM_IMM: break;
			case AM_ZPG: ram[op1] = RND(s); break;
			case AM_ZPX: zp = (u8)(op1 + x); ram[zp] = RND(s); break;
			case AM_ZPY: zp = (u8)(op1 + y); ram[zp] = RND(s); break;
			case AM_IZX: zp = (u8)(op1 + x); ram[zp] = RND(s); ram[(zp+1)&0xFF] = RND(s); addr = ram[zp] | (ram[(zp+1)&0xFF] << 8); ram[addr] = RND(s); break;
			case AM_IZY: ram[op1] = RND(s); ram[(op1+1)&0xFF] = RND(s); addr = (u16)(ram[op1] | (ram[(op1+1)&0xFF] << 8)) + y; ram[addr & 0xFFFF] = RND(s); break;
			case AM_ABS: addr = op1 | (op2 << 8); ram[addr] = RND(s); break;
			case AM_ABX: addr = (u16)((op1 | (op2 << 8)) + x); ram[addr & 0xFFFF] = RND(s); break;
			case AM_ABY: addr = (u16)((op1 | (op2 << 8)) + y); ram[addr & 0xFFFF] = RND(s); break;
			case AM_IND: addr = op1 | (op2 << 8); ram[addr] = RND(s); ram[(addr+1) & 0xFFFF] = RND(s); break;
			default: break;
			}
		}
		RAM ram0 = ram;
		cpu6502T_set(&cpu, TEST_ADDR, a, x, y, sp, p);
		u16 ipc = cpu.PC;
		u8 isp = cpu.SP, ia = cpu.A, ix = cpu.X, iy = cpu.Y, ip = cpu.P;
		if (is_kil) {
			if (t) buf += ',';
			fmt(buf, "{\"name\":\"%02x\",\"initial\":{\"pc\":%d,\"s\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"p\":%d,\"ram\":[%s]},"
			    "\"final\":{\"pc\":%d,\"s\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"p\":%d,\"ram\":[]}}\n",
			    opc, ipc, isp, ia, ix, iy, ip, json_ram(ram0).c_str(),
			    ipc + 1, isp, ia, ix, iy, ip);
			continue;
		}
		cpu6502T_log_start(&cpu);
		cpu6502T_step(&cpu);
		if (cpu.halted) { cpu6502T_log_stop(&cpu); continue; }
		int ncyc = cpu6502T_log_count(&cpu);
		const CPU6502T_Cycle *cyc = cpu6502T_log_entries(&cpu);
		u16 fpc = cpu.PC;
		cpu6502T_flush(&cpu);
		if (cpu.halted) { cpu6502T_log_stop(&cpu); continue; }
		cpu6502T_get(&cpu);
		cpu6502T_log_stop(&cpu);
		cpu.PC = fpc;
		if (t) buf += ',';
		if (info.len == 1)      fmt(buf, "{\"name\":\"%02x\",", opc);
		else if (info.len == 2) fmt(buf, "{\"name\":\"%02x %02x\",", opc, op1);
		else                    fmt(buf, "{\"name\":\"%02x %02x %02x\",", opc, op1, op2);
		fmt(buf, "\"initial\":{\"pc\":%d,\"s\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"p\":%d,\"ram\":[%s]},"
		    "\"final\":{\"pc\":%d,\"s\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"p\":%d,\"ram\":[%s]},"
		    "\"cycles\":[%s]}\n",
		    ipc, isp, ia, ix, iy, ip, json_ram(ram0).c_str(),
		    fpc, cpu.SP, cpu.A, cpu.X, cpu.Y, cpu.P, json_ram_diff(ram0, ram).c_str(),
		    json_cycles(cyc, ncyc).c_str());
	}
	buf += "\n]";
	return buf;
}

struct ThreadArg {
	int start_opc, end_opc, count;
	u64 seed;
	std::string results[256];
};

int g_done = 0;

static void thread_func(ThreadArg* arg) {
	RAM ram{};
	CPU6502T cpu;
	cpu6502T_init(&cpu, ram.data());
	for (int opc = arg->start_opc; opc < arg->end_opc; opc++) {
		arg->results[opc] = gen_opcode(ram, cpu, opc, arg->count, arg->seed + opc);
		g_done++;
	}
}

int main(int argc, char* argv[]) {
	int count = 10000;
	int nthreads = (int)std::thread::hardware_concurrency();
	u64 seed = 0;
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) { seed = std::strtoull(argv[++i], nullptr, 0); }
		else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) { nthreads = std::atoi(argv[++i]); }
		else if (!count) { /* already set */ }
		else { count = std::atoi(argv[i]); }
	}
	nthreads = std::clamp(nthreads, 1, 256);
	if (!seed) seed = (u64)std::time(nullptr) ^ 0xDEADBEEFCAFEBABEULL;
	std::printf("6502 perfect test ZIP: %d tests/opc, %d opcodes, %d threads, seed=0x%016llX\n", count, 256, nthreads, (unsigned long long)seed);
	int total = 256, opc_per = total / nthreads;
	std::vector<ThreadArg> args(nthreads);
	std::vector<std::thread> threads;
	auto t0 = std::chrono::high_resolution_clock::now();
	for (int t = 0; t < nthreads; t++) {
		args[t] = {t * opc_per, (t == nthreads - 1) ? total : (t + 1) * opc_per, count, seed};
		threads.emplace_back(thread_func, &args[t]);
	}
	while (g_done < total) {
		std::fprintf(stderr, "\r  %3d/%d opcodes", g_done, total);
		std::this_thread::sleep_for(std::chrono::milliseconds(333));
	}
	for (auto& th : threads) th.join();
	std::fprintf(stderr, "\r  %d/%d opcodes  ", total, total);
	double elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
	std::remove(ZIP_FILE);
	auto* zip = comp_zip_writer_open();
	for (int opc = 0; opc < total; opc++) {
		int t = std::min(opc / opc_per, nthreads - 1);
		char path[64]; std::snprintf(path, sizeof(path), ZIP_PATH, opc);
		comp_zip_writer_add_data(zip, path, (const u8*)args[t].results[opc].data(), (int)args[t].results[opc].size());
	}
	comp_zip_writer_close(zip, ZIP_FILE);
	std::fprintf(stderr, "  %.1fs -> %s\n", elapsed, ZIP_FILE);
	return 0;
}
