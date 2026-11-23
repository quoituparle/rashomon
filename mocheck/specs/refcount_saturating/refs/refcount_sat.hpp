// Saturating reference count (the Linux refcount_t idea): the count lives in
// BITS bits and the all-ones value is reserved as "saturated". Once saturated
// the count never moves again and release never reports "last": the object is
// leaked instead of being freed while references are still alive.
#include <atomic>

template <unsigned BITS> struct RefCount {
	static constexpr unsigned SAT = (unsigned)((1ull << BITS) - 1);
	std::atomic<unsigned> n{1};

	void acquire() {
		unsigned old = n.load(std::memory_order_relaxed);
		while (old != SAT &&
		       !n.compare_exchange_weak(old, old + 1, std::memory_order_relaxed,
		                                std::memory_order_relaxed)) {
		}
	}

	bool release() {
		unsigned old = n.load(std::memory_order_relaxed);
		for (;;) {
			if (old == SAT)
				return false; // saturated: leak
			if (n.compare_exchange_weak(old, old - 1, std::memory_order_release,
			                            std::memory_order_relaxed))
				break;
		}
		if (old == 1) {
			std::atomic_thread_fence(std::memory_order_acquire);
			return true;
		}
		return false;
	}
};
