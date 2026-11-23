// CAS on tail failed but it does not wait for the successor to link in
// MCS queue lock: each waiter spins on its own node; the lock is handed over in
// FIFO order by writing the successor's flag.
#include <atomic>

struct QLock {
	struct Node {
		std::atomic<Node *> next;
		std::atomic<bool> locked;
	};
	std::atomic<Node *> tail{nullptr};

	void lock(Node &n) {
		n.next.store(nullptr, std::memory_order_relaxed);
		n.locked.store(true, std::memory_order_relaxed);
		Node *prev = tail.exchange(&n, std::memory_order_acq_rel);
		if (prev) {
			prev->next.store(&n, std::memory_order_release);
			while (n.locked.load(std::memory_order_acquire)) {
			}
		}
	}

	void unlock(Node &n) {
		Node *succ = n.next.load(std::memory_order_acquire);
		if (!succ) {
			Node *expected = &n;
			if (tail.compare_exchange_strong(expected, nullptr, std::memory_order_release,
			                                 std::memory_order_relaxed))
				return; // nobody waiting
			return; // "tail moved, so the successor will figure it out" 
		}
		succ->locked.store(false, std::memory_order_release);
	}
};
