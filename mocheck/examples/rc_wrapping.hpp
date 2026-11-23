// Count stored in BITS bits and wrapping at 2^BITS — exactly what a uint32_t
// refcount does at BITS = 32. No overflow protection.
#include <atomic>

template <unsigned BITS> struct RefCount {
	static constexpr unsigned MASK = (unsigned)((1ull << BITS) - 1);
	std::atomic<unsigned> n{1};

	void acquire() {
		unsigned old = n.load(std::memory_order_relaxed);
		while (!n.compare_exchange_weak(old, (old + 1) & MASK, std::memory_order_relaxed,
		                                std::memory_order_relaxed)) {
		}
	}

	bool release() {
		unsigned old = n.load(std::memory_order_relaxed);
		while (!n.compare_exchange_weak(old, (old - 1) & MASK, std::memory_order_release,
		                                std::memory_order_relaxed)) {
		}
		if (old == 1) {
			std::atomic_thread_fence(std::memory_order_acquire);
			return true;
		}
		return false;
	}
};
