// Seqlock in the C++ memory model (Boehm, "Can seqlocks get along with
// programming language memory models?", MSPC 2012): relaxed atomic payload,
// release fence after the odd store, acquire fence before the re-check.
#include <atomic>

template <unsigned BITS> struct SeqLock {
	static constexpr unsigned MASK = (unsigned)((1ull << BITS) - 1);
	std::atomic<unsigned> seq{0};
	std::atomic<int> a{0}, b{0};

	void write(int x, int y) {
		unsigned s = seq.load(std::memory_order_seq_cst);
		seq.store((s + 1) & MASK, std::memory_order_seq_cst);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		a.store(x, std::memory_order_seq_cst);
		b.store(y, std::memory_order_seq_cst);
		seq.store((s + 2) & MASK, std::memory_order_seq_cst);
	}

	bool try_read(int &x, int &y) {
		unsigned s1 = seq.load(std::memory_order_seq_cst);
		if (s1 & 1)
			return false; // a write is in progress
		x = a.load(std::memory_order_seq_cst);
		y = b.load(std::memory_order_seq_cst);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		unsigned s2 = seq.load(std::memory_order_seq_cst);
		return s1 == s2;
	}
};
