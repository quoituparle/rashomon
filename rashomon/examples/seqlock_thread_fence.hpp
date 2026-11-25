// Control for seqlock_signal_fence.hpp: the same protocol with real thread fences.
#include <atomic>

template <unsigned BITS> struct SeqLock {
	static constexpr unsigned MASK = (unsigned)((1ull << BITS) - 1);
	std::atomic<unsigned> seq{0};
	std::atomic<int> a{0}, b{0};

	void write(int x, int y) {
		unsigned s = seq.load(std::memory_order_relaxed);
		seq.store((s + 1) & MASK, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acq_rel);
		a.store(x, std::memory_order_relaxed);
		b.store(y, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acq_rel);
		seq.store((s + 2) & MASK, std::memory_order_release);
	}

	bool try_read(int &x, int &y) {
		unsigned s1 = seq.load(std::memory_order_acquire);
		if (s1 & 1)
			return false;
		std::atomic_thread_fence(std::memory_order_acq_rel);
		x = a.load(std::memory_order_relaxed);
		y = b.load(std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_acq_rel);
		unsigned s2 = seq.load(std::memory_order_acquire);
		return s1 == s2;
	}
};
