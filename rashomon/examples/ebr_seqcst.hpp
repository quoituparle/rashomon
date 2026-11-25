// everything seq_cst
// Epoch-based reclamation, single retiring thread.
//  - global epoch wraps at 2^EBITS; announcements are (epoch << 1) | active
//  - a node retired in epoch e is freed when the epoch reaches e + 2
//  - 4 limbo buckets indexed by epoch & 3, which stays consistent across the
//    2^EBITS wrap because 4 divides 2^EBITS (EBITS >= 2)
//  - seq_cst fences on both sides of the store->load (Dekker) pattern:
//    reader: announce, fence, read pointer; writer: unlink, fence, scan
#include <atomic>

template <unsigned NT, unsigned EBITS> struct Epoch {
	static_assert(EBITS >= 2 && EBITS <= 32, "4 buckets must divide the epoch range");
	static constexpr unsigned MASK = (unsigned)((1ull << EBITS) - 1);
	static constexpr int SLOTS = 16;

	std::atomic<unsigned> global{0};
	std::atomic<unsigned long long> ann[NT]; // (epoch << 1) | active: needs EBITS + 1 bits
	int limbo[4][SLOTS]; // owned by the retiring thread
	int nlimbo[4];

	Epoch() {
		for (unsigned t = 0; t < NT; ++t)
			ann[t].store(0, std::memory_order_seq_cst);
		for (int b = 0; b < 4; ++b)
			nlimbo[b] = 0;
	}

	void enter(int t) {
		unsigned e = global.load(std::memory_order_seq_cst);
		// release, not relaxed: this store overwrites exit()'s release store, and in
		// C++20 a later plain store does not continue a release sequence, so a scanner
		// that reads this value must still synchronize with the previous critical section
		ann[t].store(((unsigned long long)e << 1) | 1, std::memory_order_seq_cst);
		std::atomic_thread_fence(std::memory_order_seq_cst);
	}

	void exit(int t) { ann[t].store(0, std::memory_order_seq_cst); }

	void free_bucket(unsigned b) {
		for (int i = 0; i < nlimbo[b]; ++i)
			rashomon_free(limbo[b][i]);
		nlimbo[b] = 0;
	}

	bool try_advance() {
		unsigned e = global.load(std::memory_order_seq_cst);
		std::atomic_thread_fence(std::memory_order_seq_cst);
		for (unsigned t = 0; t < NT; ++t) {
			unsigned long long a = ann[t].load(std::memory_order_seq_cst);
			if ((a & 1) && (a >> 1) != e)
				return false; // someone is still in an older epoch
		}
		unsigned ne = (e + 1) & MASK;
		if (!global.compare_exchange_strong(e, ne, std::memory_order_seq_cst, std::memory_order_seq_cst))
			return false;
		free_bucket((ne + 2) & 3); // retired in epoch ne - 2
		return true;
	}

	void retire(int, int node) {
		unsigned e = global.load(std::memory_order_seq_cst);
		unsigned b = e & 3;
		limbo[b][nlimbo[b]++] = node;
		try_advance();
	}

	void drain() {
		for (int i = 0; i < 3; ++i)
			try_advance();
	}
};
