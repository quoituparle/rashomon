// "AI" draft #1 (after the lint fix): bounded MPMC queue, one 'full' flag per slot.
// mocheck verdict: FAIL — two producers can claim the same slot, because a bool
// cannot tell "free for this lap" from "claimed this lap but not yet published".
#include <atomic>

template <unsigned CAP> struct Queue {
	struct Slot {
		std::atomic<bool> full{false};
		int value;
	};
	Slot slots[CAP];
	std::atomic<unsigned> head{0}, tail{0};

	bool push(int v) {
		unsigned t = tail.load(std::memory_order_relaxed);
		for (;;) {
			Slot &s = slots[t % CAP];
			if (s.full.load(std::memory_order_acquire))
				return false; // slot still occupied: queue is full
			if (tail.compare_exchange_weak(t, t + 1, std::memory_order_relaxed,
			                               std::memory_order_relaxed)) {
				s.value = v;
				s.full.store(true, std::memory_order_release); // publish
				return true;
			}
		}
	}

	bool pop(int &out) {
		unsigned h = head.load(std::memory_order_relaxed);
		for (;;) {
			Slot &s = slots[h % CAP];
			if (!s.full.load(std::memory_order_acquire))
				return false; // nothing published here: queue is empty
			if (head.compare_exchange_weak(h, h + 1, std::memory_order_relaxed,
			                               std::memory_order_relaxed)) {
				out = s.value;
				s.full.store(false, std::memory_order_release);
				return true;
			}
		}
	}
};
