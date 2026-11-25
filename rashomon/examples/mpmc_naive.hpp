#include <atomic>
template <unsigned CAP> struct Queue {
	int buf[CAP];
	std::atomic<unsigned> head{0}, tail{0};
	bool push(int v) {
		unsigned t = tail.load(std::memory_order_acquire);
		if (t - head.load(std::memory_order_acquire) == CAP) return false;
		t = tail.fetch_add(1, std::memory_order_acq_rel);
		buf[t % CAP] = v;
		return true;
	}
	bool pop(int &out) {
		unsigned h = head.load(std::memory_order_acquire);
		if (tail.load(std::memory_order_acquire) == h) return false;
		h = head.fetch_add(1, std::memory_order_acq_rel);
		out = buf[h % CAP];
		return true;
	}
};
