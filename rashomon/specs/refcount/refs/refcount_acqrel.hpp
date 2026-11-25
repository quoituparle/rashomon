// libstdc++-style reference count: acq_rel decrement, no fence (found by rashomon minimize).
#include <atomic>

template <unsigned BITS> struct RefCount {
	std::atomic<unsigned> n{1};

	void acquire() { n.fetch_add(1, std::memory_order_relaxed); }

	bool release() {
		if (n.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			return true;
		}
		return false;
	}
};
