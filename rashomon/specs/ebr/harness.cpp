// Tool-owned harness. The implementation under test cannot change anything here.
//
// RCU-style use of epoch-based reclamation: one writer (tid 0) repeatedly
// publishes a fresh node through a shared pointer and retires the old one;
// MO_R readers (tids 1..MO_R) enter, read the current node's (plain) payload,
// and exit. rashomon_free() poisons a node, so a reader still holding a freed
// node either races with the poisoning write or reads the poison.
//
// The Epoch object lives on the heap, constructed with plain `new` (no zeroing),
// so an implementation that relies on zero-initialised memory is caught.
//
// MO_WPIN: the writer does all its publishing inside its own read-side critical
// section (retire from within a pinned section, as RCU/EBR libraries allow) and
// at the end must still be able to read the node it pinned at the start.
//
// MO_EBITS shrinks the epoch counter. MO_PRE solo publishes (each one advances
// the epoch) push it to its wrap-around point before the concurrent part.
#include <pthread.h>
#include <atomic>
#include <cassert>

#ifndef MO_PRE
#define MO_PRE 0
#endif
#ifndef MO_WPIN
#define MO_WPIN 0
#endif
#define MO_NT (1 + MO_R)
#define MO_NODES (1 + MO_PRE + MO_W)

int payload[MO_NODES];
int freed[MO_NODES];

void rashomon_free(int node)
{
	assert(node >= 0 && node < MO_NODES && "freed something that is not a node");
	assert(!freed[node] && "double free");
	freed[node] = 1;
	payload[node] = -1; // poison
}

#include "rashomon_impl.hpp"

Epoch<MO_NT, MO_EBITS> &ebr = *new Epoch<MO_NT, MO_EBITS>; // heap, not zeroed
std::atomic<int> cur{0}; // the shared pointer (index of the current node)
int next_node = 1;       // writer-private

static void publish()
{
	int n = next_node++;
	payload[n] = 100 + n;
	int old = cur.exchange(n, std::memory_order_acq_rel);
	ebr.retire(0, old);
}

void *writer(void *)
{
	int pinned = -1;
	if (MO_WPIN) {
		ebr.enter(0);
		pinned = cur.load(std::memory_order_acquire);
	}
	for (int k = 0; k < MO_W; ++k)
		publish();
	if (MO_WPIN) {
		assert(payload[pinned] == 100 + pinned && "use after free: the writer's own pinned node was reclaimed");
		ebr.exit(0);
	}
	return nullptr;
}

void *reader(void *arg)
{
	int tid = 1 + (int)(long)arg;
	for (int k = 0; k < MO_K; ++k) {
		ebr.enter(tid);
		int n = cur.load(std::memory_order_acquire);
		int v = payload[n];
		assert(v == 100 + n && "use after free: read a node that was already reclaimed");
		ebr.exit(tid);
	}
	return nullptr;
}

int main()
{
	payload[0] = 100;
	for (int k = 0; k < MO_PRE; ++k) // solo: no reader is active, so these may be reclaimed freely
		publish();

	pthread_t w, r[MO_R];
	pthread_create(&w, nullptr, writer, nullptr);
	for (long i = 0; i < MO_R; ++i)
		pthread_create(&r[i], nullptr, reader, (void *)i);
	pthread_join(w, nullptr);
	for (int i = 0; i < MO_R; ++i)
		pthread_join(r[i], nullptr);

	ebr.drain(); // alone: everything retired must now be reclaimed
	int c = cur.load(std::memory_order_relaxed);
	for (int n = 0; n < MO_NODES; ++n) {
		if (n == c)
			assert(!freed[n] && "the current node was reclaimed");
		else
			assert(freed[n] && "a retired node was never reclaimed (even after drain)");
	}
	return 0;
}
