// Found by `mocheck minimize` from seqlock_seqcst.hpp: a different locally-minimal
// solution than Boehm's fences — release/acquire on the payload, no fences.
#include <atomic>

template <unsigned BITS> struct SeqLock {
	static constexpr unsigned MASK = (unsigned)((1ull << BITS) - 1);
	std::atomic<unsigned> seq{0};
	std::atomic<int> a{0}, b{0};

	void write(int x, int y) {
		unsigned s = seq.load(std::memory_order_relaxed);
		seq.store((s + 1) & MASK, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_relaxed);
		a.store(x, std::memory_order_release);
		b.store(y, std::memory_order_release);
		seq.store((s + 2) & MASK, std::memory_order_release);
	}

	bool try_read(int &x, int &y) {
		unsigned s1 = seq.load(std::memory_order_acquire);
		if (s1 & 1)
			return false; // a write is in progress
		x = a.load(std::memory_order_acquire);
		y = b.load(std::memory_order_acquire);
		std::atomic_thread_fence(std::memory_order_relaxed);
		unsigned s2 = seq.load(std::memory_order_relaxed);
		return s1 == s2;
	}
};
