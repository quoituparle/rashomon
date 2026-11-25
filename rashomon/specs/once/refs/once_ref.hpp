// call_once: 0 = not started, 1 = running, 2 = done. The fast path is a single
// acquire load; the loser of the CAS waits for 'done'.
#include <atomic>

struct Once {
	std::atomic<int> state{0};

	void call(void (*fn)()) {
		if (state.load(std::memory_order_acquire) == 2)
			return; // fast path
		int expected = 0;
		// failure may be relaxed: a loser does not use the value it read, it goes on to
		// wait for 'done' with acquire loads, and those provide the synchronization
		if (state.compare_exchange_strong(expected, 1, std::memory_order_relaxed,
		                                  std::memory_order_relaxed)) {
			fn();
			state.store(2, std::memory_order_release);
			return;
		}
		while (state.load(std::memory_order_acquire) != 2) {
			// someone else is running fn
		}
	}
};
