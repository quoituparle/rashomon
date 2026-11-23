// EXPERIMENT (not the published library): rigtorp/Seqlock's protocol, verbatim orderings and
// signal fences, applied to a two-word payload held in two relaxed atomics (so there is no data
// race and nothing else changes). Question: is the protocol itself sound on a weak memory model?
#include <atomic>
#include <cstddef>

template <unsigned BITS> struct SeqLock {
	std::atomic<int> va_{0}, vb_{0};
	std::atomic<std::size_t> seq_{0};

	bool try_read(int &a, int &b) {
		std::size_t seq0, seq1;
		do {
			seq0 = seq_.load(std::memory_order_acquire);
			std::atomic_signal_fence(std::memory_order_acq_rel);
			a = va_.load(std::memory_order_relaxed);
			b = vb_.load(std::memory_order_relaxed);
			std::atomic_signal_fence(std::memory_order_acq_rel);
			seq1 = seq_.load(std::memory_order_acquire);
		} while (seq0 != seq1 || seq0 & 1);
		return true;
	}

	void write(int a, int b) {
		std::size_t seq0 = seq_.load(std::memory_order_relaxed);
		seq_.store(seq0 + 1, std::memory_order_release);
		std::atomic_signal_fence(std::memory_order_acq_rel);
		va_.store(a, std::memory_order_relaxed);
		vb_.store(b, std::memory_order_relaxed);
		std::atomic_signal_fence(std::memory_order_acq_rel);
		seq_.store(seq0 + 2, std::memory_order_release);
	}
};
