// "AI" draft #2: bounded MPMC queue with a per-slot turn counter.
// For lap L of a slot: turn == 2L means free for the producer of that lap,
// turn == 2L+1 means filled for the consumer of that lap.
#include <atomic>

template <unsigned CAP> struct Queue {
	struct Slot {
		std::atomic<unsigned> turn{0};
		int value;
	};
	Slot slots[CAP];
	std::atomic<unsigned> head{0}, tail{0};

	bool push(int v) {
		unsigned t = tail.load(std::memory_order_relaxed);
		for (;;) {
			Slot &s = slots[t % CAP];
			unsigned want = 2 * (t / CAP);
			unsigned turn = s.turn.load(std::memory_order_acquire);
			if (turn == want) {
				if (tail.compare_exchange_weak(t, t + 1, std::memory_order_relaxed,
				                               std::memory_order_relaxed)) {
					s.value = v;
					s.turn.store(want + 1, std::memory_order_release);
					return true;
				}
			} else if ((int)(turn - want) < 0) {
				return false; // previous lap not consumed yet: full
			} else {
				t = tail.load(std::memory_order_relaxed); // index already taken
			}
		}
	}

	bool pop(int &out) {
		unsigned h = head.load(std::memory_order_relaxed);
		for (;;) {
			Slot &s = slots[h % CAP];
			unsigned want = 2 * (h / CAP) + 1;
			unsigned turn = s.turn.load(std::memory_order_acquire);
			if (turn == want) {
				if (head.compare_exchange_weak(h, h + 1, std::memory_order_relaxed,
				                               std::memory_order_relaxed)) {
					out = s.value;
					s.turn.store(want + 1, std::memory_order_release);
					return true;
				}
			} else if ((int)(turn - want) < 0) {
				return false; // this lap not published yet: empty
			} else {
				h = head.load(std::memory_order_relaxed); // index already taken
			}
		}
	}
};
