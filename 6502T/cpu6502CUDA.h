#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "sim.h"
#include "netlist_6502.h"

namespace cpu6502cuda {

	static constexpr int N = N_NODES_6502;
	static constexpr int MAX_ADJ = N_TRANS_6502 * 2;
	static constexpr int ND_vcc = 657;
	static constexpr int ND_vss = 558;
	static constexpr int ND_clk0 = 1171;
	static constexpr int ND_irq = 103;
	static constexpr int ND_nmi = 1297;
	static constexpr int ND_rdy = 89;
	static constexpr int ND_res = 159;
	static constexpr int ND_rw = 1156;
	static constexpr int ND_sync = 539;
	static constexpr int ND_so = 1672;
	static constexpr int ND_a[] = { 737,1234,978,162,727,858,1136,1653 };
	static constexpr int ND_x[] = { 1216,98,1,1648,85,589,448,777 };
	static constexpr int ND_y[] = { 64,1148,573,305,989,615,115,843 };
	static constexpr int ND_p[] = { 687,1444,1421,439,1119,0,77,1370 };
	static constexpr int ND_s[] = { 1403,183,81,1532,1702,1098,1212,1435 };
	static constexpr int ND_pcl[] = { 1139,1022,655,1359,900,622,377,1611 };
	static constexpr int ND_pch[] = { 1670,292,502,584,948,49,1551,205 };
	static constexpr int ND_ab[] = { 268,451,1340,211,435,736,887,1493,230,148,1443,399,1237,349,672,195 };
	static constexpr int ND_db[] = { 1005,82,945,650,1393,175,1591,1349 };

	enum Fl : uint8_t { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };

	enum class GV : uint8_t { NOTHING = 0, HI = 1, PULLUP = 2, PULLDOWN = 3, VCC = 4, VSS = 5 };
	static constexpr bool is_high(GV gv) { return gv == GV::VCC || gv == GV::PULLUP || gv == GV::HI; }

	struct NodeList;
	struct GroupList;

	class CPU {
	public:
		uint8_t A = 0, X = 0, Y = 0, P = 0, SP = 0;
		uint16_t PC = 0;
		uint8_t* ram = nullptr;
		bool halted = false;

		CPU();
		explicit CPU(uint8_t* mem);
		~CPU();

		CPU(const CPU&) = delete;
		CPU& operator=(const CPU&) = delete;

		void init(uint8_t* mem = nullptr);
		void reset();
		void irq(bool force = false) { set_node(ND_irq, 0); (void)force; }
		void irq_release() { set_node(ND_irq, 1); }	/* release IRQ line */
		void nmi() { set_node(ND_nmi, false); }

		uint8_t step() {
			uint8_t cycles = 0;
			int sync_went_low = 0;
			for (;;) {
				halfStep();
				halfStep();
				cycles++;
				bool sync = nodes_value_[ND_sync];
				if (!sync) sync_went_low = 1;
				if (sync && sync_went_low) break;
			}
			syncRegisters();
			return cycles;
		}

		void halfStepExt() { halfStep(); }

		uint8_t getNode(int node) const { return (node >= 0 && node < N) ? nodes_value_[node] : 0; }
		int getNnodes() const { return N; }
		void setNodeExt(int node, bool val) { if (node >= 0 && node < N && node != ND_vcc && node != ND_vss) set_node(node, val); }
		void softReset();
		void set(uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t sp, uint8_t p);
		void flush();
		void syncFromStruct() { syncNodesFromCpu(); }
		void syncToStruct() { syncRegisters(); }

		struct Cycle { uint16_t addr; uint8_t data; bool write; };
		void logStart() { log_active_ = true; log_len_ = 0; log_buf_.clear(); log_buf_.reserve(32); }
		void logStop() { log_active_ = false; }
		int logCount() const { return log_len_; }
		const Cycle* logEntries() const { return log_buf_.data(); }

		void gpuInit();
		uint8_t stepGPU();
		uint8_t stepGPU_fast();
		void halfStepGPU();
		void halfStepGPU_fast();
	void halfStepGPU_resident_ext() { halfStepGPU_resident(); }
		void uploadPullupPulldownGPU();
		void downloadFromGPUPublic() { downloadFromGPU(); }
		int* getDAbNodes() { return d_ab_nodes; }
		int* getDdbNodes() { return d_db_nodes; }

		#ifdef DEBUG
		void dump() const { printf("A:%02X X:%02X Y:%02X P:%02X SP:%02X PC:%04X%s\n", A, X, Y, P, SP, PC, halted ? " HALTED" : ""); }
		#endif

	private:
		uint8_t nodes_value_[N]{};
		uint8_t nodes_pullup_[N]{};
		uint8_t nodes_pulldown_[N]{};
		uint8_t* ram_heap_ = nullptr;

		int c1c2_offset_[N + 1]{};
		int vss_count_[N]{};
		int vcc_count_[N]{};
		int* h_gate = nullptr;
		int* h_other = nullptr;
		int total_adj_ = 0;

		int dep_offset_[N + 1]{};
		int dep_left_offset_[N + 1]{};
		int* dep_block_ = nullptr;
		int total_dep_ = 0;

		bool built_ = false;

		uint8_t* d_value = nullptr;
		uint8_t* d_value_out = nullptr;
		uint8_t* d_pullup = nullptr;
		uint8_t* d_pulldown = nullptr;
		int* d_c1c2_offset = nullptr;
		int* d_vss_count = nullptr;
		int* d_vcc_count = nullptr;
		int* d_gate = nullptr;
		int* d_other = nullptr;
		uint32_t* d_result = nullptr;
		int* d_changed = nullptr;
		int* d_ab_nodes = nullptr;
		int* d_db_nodes = nullptr;

		bool cuda_ready_ = false;
		int total_iters = 0;
		int max_iters = 0;
		int stab_count = 0;

		uint16_t last_addr_ = 0;
		uint8_t last_db_ = 0;
		bool last_rw_ = false;
		bool last_sync_ = false;

		bool log_active_ = false;
		int log_len_ = 0;
		std::vector<Cycle> log_buf_;

		struct NodeList* listin_ = nullptr;
		struct NodeList* listout_ = nullptr;
		struct GroupList* group_ = nullptr;

		void buildAdjacency();
		void uploadToGPU();
		void downloadFromGPU();
		void set_node(int node, bool val);
		void halfStep();
		void syncRegisters();
		void queueReg8(const int* nodes, uint8_t val);
		void syncNodesFromCpu();
		void recalcNodeCPU(int node);
		void recalcNodeListCPU();
		GV addNodeToGroupCPU(int node);
		void stabilizeGPU();
		void stabilizeGPU_noUpload();
		void halfStepGPU_resident();

		inline uint16_t readNodes(const int* nodes, int count) const {
			uint16_t result = 0;
			for (int i = count - 1; i >= 0; i--) {
				result <<= 1;
				result |= nodes_value_[nodes[i]];
			}
			return result;
		}

		inline void writeNodes(const int* nodes, int count, uint16_t v) {
			for (int i = 0; i < count; i++, v >>= 1) {
				nodes_pullup_[nodes[i]] = v & 1;
				nodes_pulldown_[nodes[i]] = !(v & 1);
			}
		}
	};

} // namespace cpu6502cuda
