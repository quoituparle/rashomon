// queue_ok.hpp with a REAL inline-asm instruction: cannot be model-checked -> INCONCLUSIVE.
#include <atomic>
template <unsigned CAP> struct Queue {
	int buf[CAP];
	std::atomic<unsigned> head, tail;
	Queue() : head(), tail() {}  // value-init of atomics lowers to memset, which GenMC rejects
	bool push(int v) {
		asm volatile("mov %0, %0" : "+r"(v));
		unsigned t = tail.load(std::memory_order_relaxed);
		if (t - head.load(std::memory_order_acquire) == CAP) return false;
		buf[t % CAP] = v;
		tail.store(t + 1, std::memory_order_release);
		return true;
	}
	bool pop(int &out) {
		unsigned h = head.load(std::memory_order_relaxed);
		if (tail.load(std::memory_order_acquire) == h) return false;
		out = buf[h % CAP];
		head.store(h + 1, std::memory_order_release);
		return true;
	}
};
