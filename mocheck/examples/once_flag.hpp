// check-then-act instead of CAS: two threads can both start init
// call_once: 0 = not started, 1 = running, 2 = done. The fast path is a single
// acquire load; the loser of the CAS waits for 'done'.
#include <atomic>

struct Once {
	std::atomic<int> state{0};

	void call(void (*fn)()) {
		if (state.load(std::memory_order_acquire) == 2)
			return; // fast path
		if (state.load(std::memory_order_acquire) == 0) { // check-then-act
			state.store(1, std::memory_order_relaxed);
			fn();
			state.store(2, std::memory_order_release);
			return;
		}
		while (state.load(std::memory_order_acquire) != 2) {
			// someone else is running fn
		}
	}
};
