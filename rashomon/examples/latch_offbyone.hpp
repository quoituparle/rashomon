// opens one count_down too early
// Single-use countdown latch (like std::latch). Every count_down is a release
// RMW, so all of them are in the release sequence the waiter acquires from.
#include <atomic>

struct Latch {
	std::atomic<int> left;
	explicit Latch(int n) : left(n) {}

	void count_down() { left.fetch_sub(1, std::memory_order_release); }

	void wait() {
		while (left.load(std::memory_order_acquire) > 1) {
		}
	}
};
