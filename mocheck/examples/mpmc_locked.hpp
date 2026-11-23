#include <atomic>
template <unsigned CAP> struct Queue {
	std::atomic<bool> locked{false};
	int buf[CAP];
	unsigned head = 0, size = 0;
	void lock() {
		bool e = false;
		while (!locked.compare_exchange_weak(e, true, std::memory_order_acquire, std::memory_order_relaxed))
			e = false;
	}
	void unlock() { locked.store(false, std::memory_order_release); }
	bool push(int v) {
		lock();
		bool ok = size < CAP;
		if (ok) { buf[(head + size) % CAP] = v; ++size; }
		unlock();
		return ok;
	}
	bool pop(int &out) {
		lock();
		bool ok = size > 0;
		if (ok) { out = buf[head]; head = (head + 1) % CAP; --size; }
		unlock();
		return ok;
	}
};
