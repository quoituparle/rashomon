// Seqlock in the C++ memory model (Boehm, "Can seqlocks get along with
// programming language memory models?", MSPC 2012): relaxed atomic payload,
// release fence after the odd store, acquire fence before the re-check.
#include <atomic>

template <unsigned BITS> struct SeqLock {
	static constexpr unsigned MASK = (unsigned)((1ull << BITS) - 1);
	std::atomic<unsigned> seq{0};
	int a = 0, b = 0;

	void write(int x, int y) {
		unsigned s = seq.load(std::memory_order_relaxed);
		seq.store((s + 1) & MASK, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		a = x;
		b = y;
		seq.store((s + 2) & MASK, std::memory_order_release);
	}

	bool try_read(int &x, int &y) {
		unsigned s1 = seq.load(std::memory_order_acquire);
		if (s1 & 1)
			return false; // a write is in progress
		x = a;
		y = b;
		std::atomic_thread_fence(std::memory_order_acquire);
		unsigned s2 = seq.load(std::memory_order_relaxed);
		return s1 == s2;
	}
};
