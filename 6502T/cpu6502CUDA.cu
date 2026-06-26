#include "cpu6502CUDA.h"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstring>
#include <chrono>

#define CUDA_CHECK(call) do { \
	cudaError_t err = call; \
	if (err != cudaSuccess) { \
	fprintf(stderr, "CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
	exit(1); \
	} \
} while(0)

namespace cpu6502cuda {

	template<int Sz>
	struct Bitmap {
		static constexpr int W = (Sz + 63) / 64;
		uint64_t d_[W]{};
		inline void set(int i) { d_[i >> 6] |= 1ULL << (i & 63); }
		inline void clr(int i) { d_[i >> 6] &= ~(1ULL << (i & 63)); }
		inline bool test(int i) const { return (d_[i >> 6] >> (i & 63)) & 1; }
		inline void set(int i, bool v) { if (v) set(i); else clr(i); }
		void zero() { for (int j = 0; j < W; j++) d_[j] = 0; }
	};

	struct NodeList {
		int buf0[N_NODES_6502]{}, buf1[N_NODES_6502]{};
		int* nodes = buf0;
		int len = 0;
		Bitmap<N_NODES_6502> bm;
		void clear() { len = 0; bm.zero(); }
		inline void add(int n) { if (!bm.test(n)) { bm.set(n); nodes[len++] = n; } }
		void swapBufs(NodeList& other) { int* tmp = nodes; nodes = other.nodes; other.nodes = tmp; }
	};

	struct GroupList {
		int nodes[N_NODES_6502]{};
		int len = 0;
		Bitmap<N_NODES_6502> bm;
		void clear() { for (int i = 0; i < len; i++) bm.clr(nodes[i]); len = 0; }
		inline void add(int n) { bm.set(n); nodes[len++] = n; }
		inline bool contains(int n) const { return bm.test(n); }
	};

	CPU::CPU() : listin_(new NodeList), listout_(new NodeList), group_(new GroupList) { init(); }
	CPU::CPU(uint8_t* mem) : listin_(new NodeList), listout_(new NodeList), group_(new GroupList) { init(mem); }

	CPU::~CPU() {
		if (cuda_ready_) {
			cudaFree(d_value);
			cudaFree(d_value_out);
			cudaFree(d_pullup);
			cudaFree(d_pulldown);
			cudaFree(d_c1c2_offset);
			cudaFree(d_vss_count);
			cudaFree(d_vcc_count);
			cudaFree(d_gate);
			cudaFree(d_other);
			cudaFree(d_result);
			cudaFree(d_changed);
			cudaFree(d_ab_nodes);
			cudaFree(d_db_nodes);
		}
		delete[] h_gate;
		delete[] h_other;
		delete[] dep_block_;
		delete[] ram_heap_;
		delete listin_;
		delete listout_;
		delete group_;
	}

	void CPU::buildAdjacency() {
		if (built_) return;

		int* t_gate = new int[N_TRANS_6502];
		int* t_c1 = new int[N_TRANS_6502];
		int* t_c2 = new int[N_TRANS_6502];
		int t_used = 0;

		for (int i = 0; i < N_TRANS_6502; i++) {
			int gate = netlist_6502_transdefs[i].gate;
			int c1 = netlist_6502_transdefs[i].c1;
			int c2 = netlist_6502_transdefs[i].c2;
			bool found = false;
			for (int j = 0; j < t_used; j++) {
				if (t_gate[j] == gate &&
					((t_c1[j] == c1 && t_c2[j] == c2) ||
						(t_c1[j] == c2 && t_c2[j] == c1))) {
					found = true; break;
				}
			}
			if (!found) {
				t_gate[t_used] = gate;
				t_c1[t_used] = c1;
				t_c2[t_used] = c2;
				t_used++;
			}
		}

		int* c1c2_count = new int[N]();
		for (int i = 0; i < t_used; i++) {
			c1c2_count[t_c1[i]]++;
			c1c2_count[t_c2[i]]++;
		}

		c1c2_offset_[0] = 0;
		for (int i = 0; i < N; i++)
			c1c2_offset_[i + 1] = c1c2_offset_[i] + c1c2_count[i];

		for (int i = 0; i < N; i++) { vss_count_[i] = 0; vcc_count_[i] = 0; }
		for (int i = 0; i < t_used; i++) {
			for (int side = 0; side < 2; side++) {
				int node = (side == 0) ? t_c1[i] : t_c2[i];
				int oth = (side == 0) ? t_c2[i] : t_c1[i];
				if (oth == ND_vss) vss_count_[node]++;
				else if (oth == ND_vcc) vcc_count_[node]++;
			}
		}

		h_gate = new int[MAX_ADJ];
		h_other = new int[MAX_ADJ];

		int* fill = new int[N];
		for (int i = 0; i < N; i++)
			fill[i] = c1c2_offset_[i] + vss_count_[i] + vcc_count_[i];
		for (int i = 0; i < N; i++) c1c2_count[i] = c1c2_offset_[i];

		for (int i = 0; i < t_used; i++) {
			for (int side = 0; side < 2; side++) {
				int node = (side == 0) ? t_c1[i] : t_c2[i];
				int oth = (side == 0) ? t_c2[i] : t_c1[i];
				if (oth == ND_vss) {
					int pos = c1c2_count[node]++;
					h_gate[pos] = t_gate[i]; h_other[pos] = oth;
				}
			}
		}
		for (int i = 0; i < t_used; i++) {
			for (int side = 0; side < 2; side++) {
				int node = (side == 0) ? t_c1[i] : t_c2[i];
				int oth = (side == 0) ? t_c2[i] : t_c1[i];
				if (oth == ND_vcc) {
					int pos = c1c2_count[node]++;
					h_gate[pos] = t_gate[i]; h_other[pos] = oth;
				}
			}
		}
		for (int i = 0; i < t_used; i++) {
			for (int side = 0; side < 2; side++) {
				int node = (side == 0) ? t_c1[i] : t_c2[i];
				int oth = (side == 0) ? t_c2[i] : t_c1[i];
				if (oth != ND_vss && oth != ND_vcc) {
					int pos = fill[node]++;
					h_gate[pos] = t_gate[i]; h_other[pos] = oth;
				}
			}
		}
		total_adj_ = c1c2_offset_[N];

		int* dep_count = new int[N]();
		int* dep_left_count = new int[N]();

		for (int i = 0; i < t_used; i++) {
			int n = t_gate[i];
			if (t_c1[i] != ND_vss && t_c1[i] != ND_vcc) dep_count[n]++;
			if (t_c2[i] != ND_vss && t_c2[i] != ND_vcc) dep_count[n]++;
			dep_left_count[n]++;
		}

		dep_offset_[0] = 0;
		for (int i = 0; i < N; i++)
			dep_offset_[i + 1] = dep_offset_[i] + dep_count[i];

		dep_left_offset_[0] = dep_offset_[N];
		for (int i = 0; i < N; i++)
			dep_left_offset_[i + 1] = dep_left_offset_[i] + dep_left_count[i];

		dep_block_ = new int[dep_offset_[N] + dep_left_offset_[N]]();
		total_dep_ = dep_offset_[N] + dep_left_offset_[N];

		int* dep_fill = new int[N]();
		int* dep_left_fill = new int[N]();

		for (int i = 0; i < t_used; i++) {
			int n = t_gate[i];
			if (t_c1[i] != ND_vss && t_c1[i] != ND_vcc) {
				int dn = t_c1[i];
				bool dup = false;
				for (int g = dep_offset_[n]; g < dep_offset_[n] + dep_fill[n]; g++)
					if (dep_block_[g] == dn) { dup = true; break; }
				if (!dup) dep_block_[dep_offset_[n] + dep_fill[n]++] = dn;
			}
			if (t_c2[i] != ND_vss && t_c2[i] != ND_vcc) {
				int dn = t_c2[i];
				bool dup = false;
				for (int g = dep_offset_[n]; g < dep_offset_[n] + dep_fill[n]; g++)
					if (dep_block_[g] == dn) { dup = true; break; }
				if (!dup) dep_block_[dep_offset_[n] + dep_fill[n]++] = dn;
			}
			{
				int left = (t_c1[i] != ND_vss && t_c1[i] != ND_vcc) ? t_c1[i] : t_c2[i];
				bool dup = false;
				for (int g = dep_left_offset_[n]; g < dep_left_offset_[n] + dep_left_fill[n]; g++)
					if (dep_block_[g] == left) { dup = true; break; }
				if (!dup) dep_block_[dep_left_offset_[n] + dep_left_fill[n]++] = left;
			}
		}

		delete[] t_gate; delete[] t_c1; delete[] t_c2;
		delete[] c1c2_count; delete[] fill;
		delete[] dep_count; delete[] dep_left_count;
		delete[] dep_fill; delete[] dep_left_fill;
		built_ = true;
	}

	void CPU::uploadToGPU() {
		if (cuda_ready_) return;
		CUDA_CHECK(cudaMalloc(&d_value, N));
		CUDA_CHECK(cudaMalloc(&d_value_out, N));
		CUDA_CHECK(cudaMalloc(&d_pullup, N));
		CUDA_CHECK(cudaMalloc(&d_pulldown, N));
		CUDA_CHECK(cudaMalloc(&d_c1c2_offset, (N + 1) * sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_vss_count, N * sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_vcc_count, N * sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_gate, total_adj_ * sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_other, total_adj_ * sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_result, 3 * sizeof(uint32_t)));
		CUDA_CHECK(cudaMalloc(&d_changed, sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_ab_nodes, 16 * sizeof(int)));
		CUDA_CHECK(cudaMalloc(&d_db_nodes, 8 * sizeof(int)));
		CUDA_CHECK(cudaMemcpy(d_ab_nodes, ND_ab, 16 * sizeof(int), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_db_nodes, ND_db, 8 * sizeof(int), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_c1c2_offset, c1c2_offset_, (N + 1) * sizeof(int), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_vss_count, vss_count_, N * sizeof(int), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_vcc_count, vcc_count_, N * sizeof(int), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_gate, h_gate, total_adj_ * sizeof(int), cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_other, h_other, total_adj_ * sizeof(int), cudaMemcpyHostToDevice));
		cuda_ready_ = true;
	}

	void CPU::downloadFromGPU() {
		if (d_value) CUDA_CHECK(cudaMemcpy(nodes_value_, d_value, N, cudaMemcpyDeviceToHost));
	}

	void CPU::init(uint8_t* mem) {
		if (!ram_heap_) ram_heap_ = new uint8_t[65536]();
		ram = mem ? mem : ram_heap_;
		halted = false;
		if (!built_) { buildAdjacency(); built_ = true; }
		memset(nodes_value_, 0, N);
		memset(nodes_pullup_, 0, N);
		memset(nodes_pulldown_, 0, N);
		for (int i = 0; i < N; i++)
			if (netlist_6502_node_is_pullup[i])
				nodes_pullup_[i] = 1;
		nodes_value_[ND_vcc] = 1;
		listout_->clear();
		set_node(ND_res, false);
		set_node(ND_clk0, true);
		set_node(ND_rdy, true);
		set_node(ND_so, false);
		set_node(ND_irq, true);
		set_node(ND_nmi, true);
		for (int i = 0; i < N; i++)
			listout_->add(i);
		recalcNodeListCPU();
	}

	void CPU::reset() {
		init(ram);
		for (int i = 0; i < 16; i++) halfStep();
		set_node(ND_res, true);
		for (int i = 0; i < 18; i++) halfStep();
		syncRegisters();
	}

	GV CPU::addNodeToGroupCPU(int node) {
		GV val = GV::NOTHING;
		if (node == ND_vss) return GV::VSS;
		if (node == ND_vcc) return GV::VCC;
		if (group_->contains(node)) return GV::NOTHING;
		const int* __restrict__ g_c1c2_offset = c1c2_offset_;
		const int* __restrict__ g_vss_count = vss_count_;
		const int* __restrict__ g_vcc_count = vcc_count_;
		const int* __restrict__ g_gate = h_gate;
		const int* __restrict__ g_other = h_other;
		const uint8_t* __restrict__ g_value = nodes_value_;
		const uint8_t* __restrict__ g_pullup = nodes_pullup_;
		const uint8_t* __restrict__ g_pulldown = nodes_pulldown_;
		int* __restrict__ g_nodes = group_->nodes;
		int& g_len = group_->len;
		group_->add(node);
		int expand_ptr = g_len - 1;
		while (expand_ptr < g_len) {
			int n = g_nodes[expand_ptr++];
			bool isPulldown = g_pulldown[n];
			bool isPullup = g_pullup[n];
			bool nodeValue = g_value[n];
			if (isPulldown && val < GV::PULLDOWN) val = GV::PULLDOWN;
			if (isPullup && val < GV::PULLUP) val = GV::PULLUP;
			if (nodeValue && val < GV::HI) val = GV::HI;
			int start = g_c1c2_offset[n];
			int end = g_c1c2_offset[n + 1];
			int vss_end = start + g_vss_count[n];
			int vcc_end = vss_end + g_vcc_count[n];
			for (int t = start; t < vss_end; t++) if (g_value[g_gate[t]]) val = GV::VSS;
			if (val != GV::VSS) {
				for (int t = vss_end; t < vcc_end; t++)
					if (g_value[g_gate[t]])
						val = GV::VCC;
				for (int t = vcc_end; t < end; t++)
					if (g_value[g_gate[t]]) {
						int o = g_other[t];
						if (!group_->contains(o)) group_->add(o);
					}
			}
			else {
				for (int t = vcc_end; t < end; t++)
					if (g_value[g_gate[t]]) {
						int o = g_other[t];
						if (!group_->contains(o)) group_->add(o);
					}
			}
		}
		return val;
	}

	void CPU::recalcNodeCPU(int node) {
		if (node == ND_vcc || node == ND_vss) return;
		group_->clear();
		GV gv = addNodeToGroupCPU(node);
		bool newv = is_high(gv);
		const int* __restrict__ g_dep_block = dep_block_;
		const int* __restrict__ g_dep_offset = dep_offset_;
		const int* __restrict__ g_dep_left_offset = dep_left_offset_;
		uint8_t* __restrict__ g_value = nodes_value_;
		int* g_grp_nodes = group_->nodes;
		int g_grp_len = group_->len;
		for (int i = 0; i < g_grp_len; i++) {
			int n = g_grp_nodes[i];
			if (n == ND_vcc || n == ND_vss) continue;
			if (g_value[n] != (newv ? 1 : 0)) {
				g_value[n] = newv ? 1 : 0;
				if (newv) {
					for (int g = g_dep_left_offset[n]; g < g_dep_left_offset[n + 1]; g++)
						listout_->add(g_dep_block[g]);
				}
				else {
					for (int g = g_dep_offset[n]; g < g_dep_offset[n + 1]; g++)
						listout_->add(g_dep_block[g]);
				}
			}
		}
	}
	void CPU::recalcNodeListCPU() {
		for (int j = 0; j < 50; j++) {
			listin_->swapBufs(*listout_);
			listin_->len = listout_->len;
			listout_->clear();
			if (listin_->len == 0) break;
			for (int i = 0; i < listin_->len; i++)
				recalcNodeCPU(listin_->nodes[i]);
		}
		listout_->clear();
	}
	void CPU::set_node(int node, bool val) {
		nodes_pullup_[node] = val ? 1 : 0;
		nodes_pulldown_[node] = val ? 0 : 1;
		listout_->add(node);
		recalcNodeListCPU();
	}
	void CPU::halfStep() {
		bool clk = nodes_value_[ND_clk0];
		set_node(ND_clk0, !clk);
		if (!clk) {
			uint16_t addr = readNodes(ND_ab, 16);
			bool rw = nodes_value_[ND_rw];
			if (rw) {
				uint16_t v = ram[addr];
				for (int i = 0; i < 8; i++, v >>= 1) {
					nodes_pullup_[ND_db[i]] = v & 1;
					nodes_pulldown_[ND_db[i]] = !(v & 1);
					listout_->add(ND_db[i]);
				}
				recalcNodeListCPU();
				if (log_active_) {
					log_buf_.push_back({ addr, static_cast<uint8_t>(ram[addr]), false });
					log_len_ = (int)log_buf_.size();
				}
			}
			else {
				uint8_t data = static_cast<uint8_t>(readNodes(ND_db, 8));
				ram[addr] = data;
				if (log_active_) {
					log_buf_.push_back({ addr, data, true });
					log_len_ = (int)log_buf_.size();
				}
			}
		}
	}
	void CPU::syncRegisters() {
		A = static_cast<uint8_t>(readNodes(ND_a, 8));
		X = static_cast<uint8_t>(readNodes(ND_x, 8));
		Y = static_cast<uint8_t>(readNodes(ND_y, 8));
		P = (static_cast<uint8_t>(readNodes(ND_p, 8)) | 0x20) & ~0x10;
		SP = static_cast<uint8_t>(readNodes(ND_s, 8));
		PC = static_cast<uint16_t>(readNodes(ND_pcl, 8) | (readNodes(ND_pch, 8) << 8));
	}
	void CPU::queueReg8(const int* nodes_arr, uint8_t val) {
		for (int i = 0; i < 8; i++, val >>= 1) {
			nodes_pullup_[nodes_arr[i]] = val & 1;
			nodes_pulldown_[nodes_arr[i]] = !(val & 1);
			listout_->add(nodes_arr[i]);
		}
	}

	void CPU::syncNodesFromCpu() {
		listout_->clear();
		queueReg8(ND_a, A);
		queueReg8(ND_x, X);
		queueReg8(ND_y, Y);
		queueReg8(ND_p, P);
		queueReg8(ND_s, SP);
		queueReg8(ND_pcl, static_cast<uint8_t>(PC));
		queueReg8(ND_pch, static_cast<uint8_t>(PC >> 8));
		recalcNodeListCPU();
	}

	void CPU::softReset() {
		halted = false;
		memset(nodes_value_, 0, N);
		memset(nodes_pullup_, 0, N);
		memset(nodes_pulldown_, 0, N);

		for (int i = 0; i < N; i++)
			if (netlist_6502_node_is_pullup[i])
				nodes_pullup_[i] = 1;

		nodes_value_[ND_vcc] = 1;

		listout_->clear();
		set_node(ND_res, false);
		set_node(ND_clk0, true);
		set_node(ND_rdy, true);
		set_node(ND_so, false);
		set_node(ND_irq, true);
		set_node(ND_nmi, true);

		for (int i = 0; i < N; i++)
			listout_->add(i);
		recalcNodeListCPU();

		for (int i = 0; i < 16; i++) halfStep();
		set_node(ND_res, true);
		for (int i = 0; i < 18; i++) halfStep();
		syncRegisters();
	}

	void CPU::set(uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t sp, uint8_t p) {
		uint8_t* r = ram;

		// RTI pops 3 bytes (P, PClo, PChi), incrementing SP by 3. TXS loads (sp-3) so that after RTI, SP = sp.
		uint8_t sp_base = static_cast<uint8_t>(sp - 3);
		uint16_t sa = 0x0100 + static_cast<uint8_t>(sp - 2); // P on stack
		uint16_t sb = 0x0100 + static_cast<uint8_t>(sp - 1); // PC lo
		uint16_t sc = 0x0100 + sp; // PC hi

		// Save clobbered regions
		uint8_t save_vec[2] = { r[0xFFFC], r[0xFFFD] };
		uint8_t save_code[11];
		uint8_t save_stack[256];
		memcpy(save_code, &r[0x0200], 11);
		memcpy(save_stack, &r[0x0100], 256);

		// Write boot loader
		r[0xFFFC] = 0x00; r[0xFFFD] = 0x02; // reset vector → $0200
		r[0x0200] = 0xEA; // NOP
		r[0x0201] = 0xA9; r[0x0202] = a; // LDA #a
		r[0x0203] = 0xA2; r[0x0204] = sp_base; // LDX #(sp-3)
		r[0x0205] = 0x9A; // TXS
		r[0x0206] = 0xA2; r[0x0207] = x; // LDX #x
		r[0x0208] = 0xA0; r[0x0209] = y; // LDY #y
		r[0x020A] = 0x40; // RTI
		r[sa] = p;
		r[sb] = static_cast<uint8_t>(pc & 0xFF);
		r[sc] = static_cast<uint8_t>(pc >> 8);

		// Run boot: soft_reset + 7 instructions
		softReset();
		for (int i = 0; i < 7; i++) step();

		// Restore clobbered RAM
		r[0xFFFC] = save_vec[0]; r[0xFFFD] = save_vec[1];
		memcpy(&r[0x0200], save_code, 11);
		memcpy(&r[0x0100], save_stack, 256);
	}

	void CPU::flush() {
		halfStep();
		halfStep();
	}

	__global__ void stabilize_one_iter_kernel(
		const uint8_t* __restrict__ value_in,
		uint8_t* __restrict__ value_out,
		const uint8_t* __restrict__ pullup,
		const uint8_t* __restrict__ pulldown,
		const int* __restrict__ c1c2_offset,
		const int* __restrict__ vss_count,
		const int* __restrict__ vcc_count,
		const int* __restrict__ gate,
		const int* __restrict__ other,
		int n_nodes)
	{
		int node = blockIdx.x * blockDim.x + threadIdx.x;
		if (node >= n_nodes) return;

		const int ND_VCC = 657;
		const int ND_VSS = 558;

		if (node == ND_VCC) { value_out[node] = 1; return; }
		if (node == ND_VSS) { value_out[node] = 0; return; }

		uint64_t visited[27] = {};
		int stack[256];
		int stack_ptr = 0;
		int expand_ptr = 0;
		bool overflow = false;

		int gv = 0;

		visited[node >> 6] = 1ULL << (node & 63);
		stack[stack_ptr++] = node;

		while (expand_ptr < stack_ptr && !overflow) {
			int n = stack[expand_ptr++];

			if (pulldown[n] && gv < 3) gv = 3;
			if (pullup[n] && gv < 2) gv = 2;
			if (value_in[n] && gv < 1) gv = 1;

			int start = c1c2_offset[n];
			int end = c1c2_offset[n + 1];
			int vss_end = start + vss_count[n];
			int vcc_end = vss_end + vcc_count[n];

			for (int t = start; t < vss_end; t++)
				if (value_in[gate[t]]) gv = 5;

			for (int t = vss_end; t < vcc_end; t++)
				if (value_in[gate[t]] && gv != 5) gv = 4;

			for (int t = vcc_end; t < end; t++) {
				if (value_in[gate[t]]) {
					int o = other[t];
					int word = o >> 6;
					int bit = o & 63;
					if (!((visited[word] >> bit) & 1)) {
						visited[word] |= 1ULL << bit;
						if (stack_ptr < 256)
							stack[stack_ptr++] = o;
						else
							overflow = true;
					}
				}
			}
		}

		bool new_high = (gv == 4 || gv == 2 || gv == 1);
		value_out[node] = new_high ? 1 : 0;
	}

	__global__ void set_node_gpu_kernel(
		uint8_t* pullup, uint8_t* pulldown,
		int node, uint8_t pu_val, uint8_t pd_val)
	{
		pullup[node] = pu_val;
		pulldown[node] = pd_val;
	}

	__global__ void set_nodes_gpu_kernel(
		uint8_t* pullup, uint8_t* pulldown,
		const int* nodes, int count, uint16_t value)
	{
		int i = blockIdx.x * blockDim.x + threadIdx.x;
		if (i < count) {
			int bit = (value >> i) & 1;
			pullup[nodes[i]] = bit;
			pulldown[nodes[i]] = 1 - bit;
		}
	}

	__global__ void extract_result_kernel(
		const uint8_t* __restrict__ value,
		const int* __restrict__ ab_nodes,
		const int* __restrict__ db_nodes,
		int rw_node, int sync_node,
		uint32_t* result)
	{
		uint32_t addr = 0;
		for (int i = 15; i >= 0; i--)
			addr = (addr << 1) | value[ab_nodes[i]];
		result[0] = addr;
		result[1] = (value[rw_node] << 8) | value[sync_node];
		uint32_t db = 0;
		for (int i = 0; i < 8; i++)
			db |= (value[db_nodes[i]] << i);
		result[2] = db;
	}

	/* Check if any node changed between old and new value buffers.
	   Sets changed[0] = 1 if any differ, 0 if converged. */
	__global__ void check_convergence_kernel(
		const uint8_t* __restrict__ old_val,
		const uint8_t* __restrict__ new_val,
		int n_nodes,
		int* changed)
	{
		int i = blockIdx.x * blockDim.x + threadIdx.x;
		if (i < n_nodes && old_val[i] != new_val[i])
			*changed = 1;
	}

	void CPU::gpuInit() {
		uploadToGPU();
		CUDA_CHECK(cudaMemcpy(d_value, nodes_value_, N, cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_pullup, nodes_pullup_, N, cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_pulldown, nodes_pulldown_, N, cudaMemcpyHostToDevice));
	}

	void CPU::stabilizeGPU() {
		if (!cuda_ready_) gpuInit();
		CUDA_CHECK(cudaMemcpy(d_pullup, nodes_pullup_, N, cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_pulldown, nodes_pulldown_, N, cudaMemcpyHostToDevice));
		int threads = 256;
		int blocks = (N + threads - 1) / threads;
		for (int batch = 0; batch < 20; batch++) {
			for (int j = 0; j < 5; j++) {
				stabilize_one_iter_kernel<<<blocks, threads>>>(
					d_value, d_value_out, d_pullup, d_pulldown,
				d_c1c2_offset, d_vss_count, d_vcc_count, d_gate, d_other, N);
				uint8_t* tmp = d_value;
				d_value = d_value_out;
			d_value_out = tmp;
			}
			int h_changed = 0;
			CUDA_CHECK(cudaMemset(d_changed, 0, sizeof(int)));
			check_convergence_kernel<<<blocks, threads>>>(d_value_out, d_value, N, d_changed);
			CUDA_CHECK(cudaMemcpy(&h_changed, d_changed, sizeof(int), cudaMemcpyDeviceToHost));
			if (!h_changed) break;
		}
		CUDA_CHECK(cudaMemcpy(nodes_value_, d_value, N, cudaMemcpyDeviceToHost));
	}

	void CPU::stabilizeGPU_noUpload() {
		if (!cuda_ready_) gpuInit();
		int threads = 256;
		int blocks = (N + threads - 1) / threads;
		for (int batch = 0; batch < 20; batch++) {
			for (int j = 0; j < 5; j++) {
				stabilize_one_iter_kernel<<<blocks, threads>>>(
					d_value, d_value_out, d_pullup, d_pulldown,
					d_c1c2_offset, d_vss_count, d_vcc_count, d_gate, d_other, N);
				uint8_t* tmp = d_value;
				d_value = d_value_out;
				d_value_out = tmp;
			}
			int h_changed = 0;
			CUDA_CHECK(cudaMemset(d_changed, 0, sizeof(int)));
			check_convergence_kernel<<<blocks, threads>>>(d_value_out, d_value, N, d_changed);
			CUDA_CHECK(cudaMemcpy(&h_changed, d_changed, sizeof(int), cudaMemcpyDeviceToHost));
			if (!h_changed) break;
		}
	}

	void CPU::uploadPullupPulldownGPU() {
		if (!cuda_ready_) gpuInit();
		CUDA_CHECK(cudaMemcpy(d_pullup, nodes_pullup_, N, cudaMemcpyHostToDevice));
		CUDA_CHECK(cudaMemcpy(d_pulldown, nodes_pulldown_, N, cudaMemcpyHostToDevice));
	}

	void CPU::halfStepGPU_fast() {
		if (!cuda_ready_) gpuInit();
		bool clk = nodes_value_[ND_clk0];
		nodes_pullup_[ND_clk0] = !clk ? 1 : 0;
		nodes_pulldown_[ND_clk0] = !clk ? 0 : 1;
		stabilizeGPU();
		if (!clk) {
			uint16_t addr = readNodes(ND_ab, 16);
			if (nodes_value_[ND_rw]) {
				uint16_t v = ram[addr];
				for (int i = 0; i < 8; i++, v >>= 1) {
					nodes_pullup_[ND_db[i]] = v & 1;
					nodes_pulldown_[ND_db[i]] = !(v & 1);
				}
				stabilizeGPU();
			}
			else {
				ram[addr] = static_cast<uint8_t>(readNodes(ND_db, 8));
			}
		}
	}

	void CPU::halfStepGPU_resident() {
		if (!cuda_ready_) gpuInit();

		// 1. Toggle CLK0 on GPU (1 thread, 0 bytes transfer)
		bool clk = nodes_value_[ND_clk0];
		uint8_t clk_pu = !clk ? 1 : 0;
		uint8_t clk_pd = !clk ? 0 : 1;
		set_node_gpu_kernel<<<1,1>>>(d_pullup, d_pulldown, ND_clk0, clk_pu, clk_pd);
		nodes_pullup_[ND_clk0] = clk_pu;
		nodes_pulldown_[ND_clk0] = clk_pd;

		// 2. Stabilize (100 Jacobi iterations, 0 bytes transfer)
		stabilizeGPU_noUpload();

		// 3. Extract addr/rw/sync/db (12 bytes D2H)
		uint32_t result[3];
		extract_result_kernel<<<1,1>>>(d_value, d_ab_nodes, d_db_nodes, ND_rw, ND_sync, d_result);
		CUDA_CHECK(cudaMemcpy(result, d_result, 3 * sizeof(uint32_t), cudaMemcpyDeviceToHost));

		uint16_t addr = (uint16_t)result[0];
		bool rw = (result[1] >> 8) & 1;
		bool sync = result[1] & 1;
		uint8_t db_val = (uint8_t)result[2];

		// 4. Phi2 processing
		if (!clk) {
			if (rw) {
				// Read: feed RAM data to DB on GPU (8 threads, 0 bytes transfer)
				uint16_t v = ram[addr];
				set_nodes_gpu_kernel<<<1,8>>>(d_pullup, d_pulldown, d_db_nodes, 8, v);
				for (int i = 0; i < 8; i++, v >>= 1) {
					nodes_pullup_[ND_db[i]] = v & 1;
					nodes_pulldown_[ND_db[i]] = !(v & 1);
				}
				stabilizeGPU_noUpload();
			} else {
				// Write: store to host RAM
				ram[addr] = db_val;
			}
		}

		// Update host mirrors for next half-step's CLK0 read
		nodes_value_[ND_clk0] = clk_pu;
		nodes_value_[ND_rw] = rw ? 1 : 0;
		nodes_value_[ND_sync] = sync ? 1 : 0;
		last_addr_ = addr;
		last_db_ = db_val;
	}

	void CPU::halfStepGPU() {
		bool clk = nodes_value_[ND_clk0];
		nodes_pullup_[ND_clk0] = !clk ? 1 : 0;
		nodes_pulldown_[ND_clk0] = !clk ? 0 : 1;
		stabilizeGPU();
		if (!clk) {
			uint16_t addr = readNodes(ND_ab, 16);
			if (nodes_value_[ND_rw]) {
				uint16_t v = ram[addr];
				for (int i = 0; i < 8; i++, v >>= 1) {
					nodes_pullup_[ND_db[i]] = v & 1;
					nodes_pulldown_[ND_db[i]] = !(v & 1);
				}
				stabilizeGPU();
			}
			else {
				ram[addr] = static_cast<uint8_t>(readNodes(ND_db, 8));
			}
		}
	}

	uint8_t CPU::stepGPU() {
		uint8_t cycles = 0;
		int sync_went_low = 0;
		for (;;) {
			halfStepGPU();
			halfStepGPU();
			cycles++;
			bool sync = nodes_value_[ND_sync];
			if (!sync) sync_went_low = 1;
			if (sync && sync_went_low) break;
		}
		syncRegisters();
		return cycles;
	}

	uint8_t CPU::stepGPU_fast() {
		if (!cuda_ready_) {
			gpuInit();
		} else {
			/* Sync host state to GPU after CPU-only reset/halfStep */
			CUDA_CHECK(cudaMemcpy(d_pullup, nodes_pullup_, N, cudaMemcpyHostToDevice));
			CUDA_CHECK(cudaMemcpy(d_pulldown, nodes_pulldown_, N, cudaMemcpyHostToDevice));
			CUDA_CHECK(cudaMemcpy(d_value, nodes_value_, N, cudaMemcpyHostToDevice));
		}
		uint8_t cycles = 0;
		int sync_went_low = 0;
		for (;;) {
			halfStepGPU_resident();
			halfStepGPU_resident();
			cycles++;
			bool sync = nodes_value_[ND_sync];
			if (!sync) sync_went_low = 1;
			if (sync && sync_went_low) break;
			if (cycles >= 100) { halted = true; break; }
		}
		/* Final register readback */
		CUDA_CHECK(cudaMemcpy(nodes_value_, d_value, N, cudaMemcpyDeviceToHost));
		syncRegisters();
		return cycles;
	}

} // namespace cpu6502T
