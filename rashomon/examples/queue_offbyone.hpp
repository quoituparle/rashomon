#include <atomic>
template <unsigned CAP> struct Queue {
	int buf[CAP];
	std::atomic<unsigned> head{0}, tail{0};
	bool push(int v) {
		unsigned t = tail.load(std::memory_order_relaxed);
		if (t - head.load(std::memory_order_acquire) == CAP) return false;
		buf[t % CAP] = v;
		tail.store(t + 1, std::memory_order_release);
		return true;
	}
	bool pop(int &out) {
		unsigned h = head.load(std::memory_order_relaxed);
		if (tail.load(std::memory_order_acquire) - h < 2) return false;
		out = buf[h % CAP];
		head.store(h + 1, std::memory_order_release);
		return true;
	}
};
