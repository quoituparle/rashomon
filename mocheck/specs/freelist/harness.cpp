// Tool-owned harness. The implementation under test cannot change anything here.
//
// A pool of MO_CAP node indices, all free at start. Two kinds of threads:
//   cycler  (MO_C of them): MO_K times { i = alloc(); use i; free(i); }
//   hoarder (MO_H of them): x = alloc(); y = alloc(); use both; free(x); free(y);
// A hoarder is exactly the B of the classic ABA story: it pops x, pops y and
// pushes x back while a cycler may be stalled between reading head and its CAS.
//
// "use i" writes and re-reads a plain int owned by node i: two threads owning
// the same node at once is a data race. At the end, run alone, the pool must
// hand out exactly CAP distinct nodes and then report empty.
//
// MO_TAG shrinks the ABA tag. MO_PRE solo alloc/free cycles advance the tag to
// its wrap-around point before the concurrent part starts.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "mocheck_impl.hpp"

#ifndef MO_PRE
#define MO_PRE 0
#endif
#ifndef MO_H
#define MO_H 0
#endif

static_assert(MO_TAG >= 16 || MO_C * MO_K + 2 * MO_H < (1u << MO_TAG),
              "premise: fewer than 2^TAG allocations may overlap one allocation");

FreeList<MO_CAP, MO_TAG> fl;
int payload[MO_CAP];

static void use(int i, int who)
{
	assert(i >= 0 && i < MO_CAP && "alloc returned an index outside the pool");
	payload[i] = who;
	assert(payload[i] == who && "node is owned by two threads at once");
}

void *cycler(void *arg)
{
	int who = 1 + (int)(long)arg;
	for (int k = 0; k < MO_K; ++k) {
		int i = fl.alloc();
		if (i >= 0) {
			use(i, who);
			fl.free(i);
		}
	}
	return nullptr;
}

void *hoarder(void *arg)
{
	int who = 100 + (int)(long)arg;
	int x = fl.alloc();
	int y = fl.alloc();
	if (x >= 0)
		use(x, who);
	if (y >= 0)
		use(y, who);
	if (x >= 0)
		fl.free(x);
	if (y >= 0)
		fl.free(y);
	return nullptr;
}

static void solo_drain_and_refill()
{
	int got[MO_CAP];
	for (int k = 0; k < MO_CAP; ++k) {
		got[k] = fl.alloc();
		assert(got[k] >= 0 && got[k] < MO_CAP && "run alone, a non-empty pool must hand out a node");
		for (int j = 0; j < k; ++j)
			assert(got[j] != got[k] && "the same node was handed out twice");
	}
	assert(fl.alloc() == -1 && "run alone, an exhausted pool must report empty");
	for (int k = 0; k < MO_CAP; ++k)
		fl.free(got[k]);
}

int main()
{
	solo_drain_and_refill();
	for (int k = 0; k < MO_PRE; ++k) { // advance the tag towards its wrap-around point
		int i = fl.alloc();
		assert(i >= 0 && "run alone, a non-empty pool must hand out a node");
		fl.free(i);
	}

	pthread_t th[MO_C + MO_H];
	for (long t = 0; t < MO_C; ++t)
		pthread_create(&th[t], nullptr, cycler, (void *)t);
	for (long t = 0; t < MO_H; ++t)
		pthread_create(&th[MO_C + t], nullptr, hoarder, (void *)t);
	for (int t = 0; t < MO_C + MO_H; ++t)
		pthread_join(th[t], nullptr);

	solo_drain_and_refill(); // nothing lost, nothing duplicated, nothing corrupted
	return 0;
}
