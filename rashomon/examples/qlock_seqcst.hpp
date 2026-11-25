// everything seq_cst
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
		n.next.store(nullptr, std::memory_order_seq_cst);
		n.locked.store(true, std::memory_order_seq_cst);
		Node *prev = tail.exchange(&n, std::memory_order_seq_cst);
		if (prev) {
			prev->next.store(&n, std::memory_order_seq_cst);
			while (n.locked.load(std::memory_order_seq_cst)) {
			}
		}
	}

	void unlock(Node &n) {
		Node *succ = n.next.load(std::memory_order_seq_cst);
		if (!succ) {
			Node *expected = &n;
			if (tail.compare_exchange_strong(expected, nullptr, std::memory_order_seq_cst,
			                                 std::memory_order_seq_cst))
				return; // nobody waiting
			while (!(succ = n.next.load(std::memory_order_seq_cst))) {
				// a successor swapped itself into tail but has not linked yet
			}
		}
		succ->locked.store(false, std::memory_order_seq_cst);
	}
};
