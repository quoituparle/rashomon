// Dmitry Vyukov's bounded MPMC queue
#include <atomic>
template <unsigned CAP> struct Queue {
	struct Cell {
		std::atomic<unsigned> seq;
		int data;
	};
	Cell cells[CAP];
	std::atomic<unsigned> enq{0}, deq{0};

	Queue() {
		for (unsigned i = 0; i < CAP; ++i)
			cells[i].seq.store(i, std::memory_order_relaxed);
	}

	bool push(int v) {
		unsigned pos = enq.load(std::memory_order_seq_cst);
		for (;;) {
			Cell &c = cells[pos % CAP];
			unsigned seq = c.seq.load(std::memory_order_seq_cst);
			int dif = (int)seq - (int)pos;
			if (dif == 0) {
				if (enq.compare_exchange_weak(pos, pos + 1, std::memory_order_seq_cst,
				                              std::memory_order_seq_cst)) {
					c.data = v;
					c.seq.store(pos + 1, std::memory_order_seq_cst);
					return true;
				}
			} else if (dif < 0) {
				return false; // full
			} else {
				pos = enq.load(std::memory_order_seq_cst);
			}
		}
	}

	bool pop(int &out) {
		unsigned pos = deq.load(std::memory_order_seq_cst);
		for (;;) {
			Cell &c = cells[pos % CAP];
			unsigned seq = c.seq.load(std::memory_order_seq_cst);
			int dif = (int)seq - (int)(pos + 1);
			if (dif == 0) {
				if (deq.compare_exchange_weak(pos, pos + 1, std::memory_order_seq_cst,
				                              std::memory_order_seq_cst)) {
					out = c.data;
					c.seq.store(pos + CAP, std::memory_order_seq_cst);
					return true;
				}
			} else if (dif < 0) {
				return false; // empty
			} else {
				pos = deq.load(std::memory_order_seq_cst);
			}
		}
	}
};
