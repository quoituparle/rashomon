#include <atomic>
struct Mutex {
	std::atomic<unsigned> next{0}, serving{0};
	void lock() {
		unsigned my = next.fetch_add(1, std::memory_order_relaxed);
		while (serving.load(std::memory_order_acquire) != my) {
		}
		std::atomic_thread_fence(std::memory_order_relaxed);
	}
	void unlock() {
		unsigned s = serving.load(std::memory_order_relaxed);
		serving.store(s + 1, std::memory_order_release);
	}
};
