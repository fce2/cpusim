// 6502 tester (Tom Harte style)

#include "cpu6502.h" // the cpu to test
#include "compress.h"
#include "json.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define ZIP_FILE "65x02-perfect.zip"
#define ZIP_PATH "65x02-main/6502/v1/%02x.json"
#define MAX_RAM 512

struct State {
	uint16_t pc;
	uint8_t  s, a, x, y, p;
	int ram_n;
	uint16_t ram_addr[MAX_RAM];
	uint8_t  ram_val[MAX_RAM];
};

static State parse_state(const Json& obj) {
	State st = {};
	st.pc = (uint16_t)obj["pc"].as_int();
	st.s  =  (uint8_t)obj["s"].as_int();
	st.a  =  (uint8_t)obj["a"].as_int();
	st.x  =  (uint8_t)obj["x"].as_int();
	st.y  =  (uint8_t)obj["y"].as_int();
	st.p  =  (uint8_t)obj["p"].as_int();
	const Json& ram = obj["ram"];
	if (ram.type() == Json::Array) {
		st.ram_n = (int)ram.size();
		if (st.ram_n > MAX_RAM) st.ram_n = MAX_RAM;
		for (int i = 0; i < st.ram_n; i++) {
			const Json& e = ram[i];
			st.ram_addr[i] = (uint16_t)e[0].as_int();
			st.ram_val[i]  =  (uint8_t)e[1].as_int();
		}
	}
	return st;
}

int main(int argc, char* argv[]) {
	const char* zipfile = ZIP_FILE;
	for (int i = 1; i < argc; i++) { if (argv[i][0] != '-') zipfile = argv[i]; }
	ZipFile zf;
	if (comp_zip_open(&zf, zipfile) < 0) {
		fprintf(stderr, "Cannot open %s\n", zipfile);
		fprintf(stderr, "Usage: mainT005 [zipfile]\n");
		return 1;
	}
	static uint8_t ram[65536];
	CPU6502 cpu;
	cpu6502_init(&cpu, ram, NULL, NULL);
	cpu6502_reset(&cpu);
	int total_pass = 0, total_fail = 0, total_skip = 0;
	for (int opc = 0; opc < 256; opc++) {
		char path[64];
		snprintf(path, sizeof(path), ZIP_PATH, opc);
		int idx = comp_zip_find(&zf, path);
		if (idx < 0) continue;
		int size = zf.entries[idx].uncompressed_size;
		if (size <= 0 || size > 64 * 1024 * 1024) continue;
		uint8_t* buf = (uint8_t*)malloc(size + 1);
		if (!buf) continue;
		int got = comp_zip_read_entry(&zf, idx, buf, size);
		if (got < 0) { free(buf); continue; }
		buf[got] = '\0';
		std::string json_str((const char*)buf, got);
		free(buf);
		Json tests = Json::parse(json_str);
		if (tests.type() != Json::Array) continue;
		int pass = 0, fail = 0, skip = 0;
		for (size_t t = 0; t < tests.size(); t++) {
			const Json& entry = tests[t];
			bool has_final = (entry["final"].type() != Json::Null);
			memset(ram, 0, 65536);
			State initial = parse_state(entry["initial"]);
			for (int i = 0; i < initial.ram_n; i++) ram[initial.ram_addr[i]] = initial.ram_val[i];
			cpu.A = initial.a;
			cpu.X = initial.x;
			cpu.Y = initial.y;
			cpu.P = initial.p;
			cpu.SP = initial.s;
			cpu.PC = initial.pc;
			cpu.halted = 0;
			if (!has_final) { skip++; continue; }
			cpu6502_step(&cpu);
			State final_s = parse_state(entry["final"]);
			int errs = 0;
			if (cpu.A  != final_s.a)  errs++;
			if (cpu.X  != final_s.x)  errs++;
			if (cpu.Y  != final_s.y)  errs++;
			if (cpu.P  != final_s.p)  errs++;
			if (cpu.SP != final_s.s)  errs++;
			if (cpu.PC != final_s.pc) errs++;
			for (int i = 0; i < final_s.ram_n; i++) { if (ram[final_s.ram_addr[i]] != final_s.ram_val[i]) errs++; }
			if (errs == 0) pass++;
			else fail++;
		}
		total_pass += pass;
		total_fail += fail;
		total_skip += skip;
		fprintf(stderr, "\r  %3d/256  %d pass  %d fail  %d skip", opc + 1, total_pass, total_fail, total_skip);
		if (fail > 0) printf("  %02X: %d pass, %d fail, %d skip\n", opc, pass, fail, skip);
	}
	comp_zip_close(&zf);
	fprintf(stderr, "\r  256/256  %d pass  %d fail  %d skip     \n", total_pass, total_fail, total_skip);
	printf("========================================\n");
	printf("6502 perfect ZIP test: %d passed, %d failed, %d skipped\n", total_pass, total_fail, total_skip);
	if (total_fail == 0) printf("ALL TESTS PASS!\n");
	printf("========================================\n");
	return total_fail > 0 ? 1 : 0;
}
