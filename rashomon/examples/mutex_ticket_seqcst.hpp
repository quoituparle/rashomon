#include <atomic>
struct Mutex {
	std::atomic<unsigned> next{0}, serving{0};
	void lock() {
		unsigned my = next.fetch_add(1, std::memory_order_seq_cst);
		while (serving.load(std::memory_order_seq_cst) != my) {
		}
		std::atomic_thread_fence(std::memory_order_seq_cst);
	}
	void unlock() {
		unsigned s = serving.load(std::memory_order_seq_cst);
		serving.store(s + 1, std::memory_order_seq_cst);
	}
};
