// Tagged Treiber stack over node indices. head packs (tag << 16) | (index + 1);
// index + 1 == 0 means empty. Every successful alloc bumps the tag (masked to
// TAG bits), so a head that "comes back" to the same node has a different tag.
#include <atomic>

template <unsigned CAP, unsigned TAG> struct FreeList {
	static_assert(CAP < 0xFFFF && TAG <= 16, "index and tag share one 32-bit word");
	static constexpr unsigned TAG_MASK = (unsigned)((1ull << TAG) - 1);

	std::atomic<unsigned> head;
	std::atomic<int> next[CAP];

	static unsigned pack(unsigned tag, int idx) { return ((tag & TAG_MASK) << 16) | (unsigned)(idx + 1); }
	static int index_of(unsigned h) { return (int)(h & 0xFFFF) - 1; }

	FreeList() {
		for (int i = 0; i < (int)CAP; ++i)
			next[i].store(i + 1 < (int)CAP ? i + 1 : -1, std::memory_order_relaxed);
		head.store(pack(0, 0), std::memory_order_relaxed);
	}

	int alloc() {
		unsigned h = head.load(std::memory_order_acquire);
		for (;;) {
			int i = index_of(h);
			if (i < 0)
				return -1;
			int n = next[i].load(std::memory_order_relaxed);
			if (head.compare_exchange_weak(h, pack(h >> 16, n), std::memory_order_acquire,
			                               std::memory_order_acquire))
				return i;
		}
	}

	void free(int i) {
		unsigned h = head.load(std::memory_order_relaxed);
		for (;;) {
			next[i].store(index_of(h), std::memory_order_relaxed);
			if (head.compare_exchange_weak(h, pack(h >> 16, i), std::memory_order_release,
			                               std::memory_order_relaxed))
				return;
		}
	}
};
