#include <atomic>
template <unsigned CAP> struct Queue {
	int buf[CAP];
	std::atomic<unsigned> head{0}, tail{0};
	bool push(int v) {
		unsigned t = tail.load(std::memory_order_seq_cst);
		if (t - head.load(std::memory_order_seq_cst) == CAP) return false;
		buf[t % CAP] = v;
		tail.store(t + 1, std::memory_order_seq_cst);
		return true;
	}
	bool pop(int &out) {
		unsigned h = head.load(std::memory_order_seq_cst);
		if (tail.load(std::memory_order_seq_cst) == h) return false;
		out = buf[h % CAP];
		head.store(h + 1, std::memory_order_seq_cst);
		return true;
	}
};
