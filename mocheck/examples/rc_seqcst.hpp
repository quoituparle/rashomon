// Standard reference count (the shared_ptr / Boost scheme): relaxed increment,
// release decrement, acquire fence only on the path that destroys.
#include <atomic>

template <unsigned BITS> struct RefCount {
	std::atomic<unsigned> n{1};

	void acquire() { n.fetch_add(1, std::memory_order_seq_cst); }

	bool release() {
		if (n.fetch_sub(1, std::memory_order_seq_cst) == 1) {
			std::atomic_thread_fence(std::memory_order_seq_cst);
			return true;
		}
		return false;
	}
};
