#include <atomic>
template <unsigned CAP> struct Queue {
	int buf[CAP];
	std::atomic<unsigned> head{0}, tail{0};
	bool push(int v) {
		unsigned t = tail.load(std::memory_order_relaxed);
		if (t - head == CAP) return false;      // implicit load
		buf[t % CAP] = v;
		tail++;                                 // implicit RMW
		return true;
	}
	bool pop(int &out) {
		unsigned h = head.load();               // defaulted order
		if (tail.load(std::memory_order_acquire) == h) return false;
		out = buf[h % CAP];
		head.store(h + 1, std::memory_order_acquire);  // invalid for a store
		return true;
	}
};
