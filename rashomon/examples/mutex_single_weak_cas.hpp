#include <atomic>
struct Mutex {
	std::atomic<bool> locked{false};
	bool try_lock() { bool e = false; return locked.compare_exchange_weak(e, true, std::memory_order_acquire, std::memory_order_relaxed); }
	void lock() { while (!try_lock()) { } }
	void unlock() { locked.store(false, std::memory_order_release); }
};
