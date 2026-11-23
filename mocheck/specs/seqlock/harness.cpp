// Tool-owned harness. The implementation under test cannot change anything here.
//
// One writer publishes pairs (k, 7k) for increasing k; readers call try_read.
// A successful read must return a pair that was actually written (never a mix of
// two writes), never go backwards for the same reader, and never be older than
// what the reader is guaranteed to see. After join, a read must succeed and see
// the last write.
//
// MO_BITS shrinks the sequence counter; MO_PRE solo writes push it close to the
// wrap-around point, so a wrap that would take 2^31 writes in production happens
// within a handful of steps here.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "mocheck_impl.hpp"

#ifndef MO_PRE
#define MO_PRE 0
#endif

static_assert(MO_BITS >= 32 || MO_W < (1u << (MO_BITS - 1)),
              "seqlock premise: fewer than 2^(BITS-1) writes may overlap one read");

SeqLock<MO_BITS> L;
int got[MO_R][MO_K];

void *writer(void *)
{
	for (int k = MO_PRE + 1; k <= MO_PRE + MO_W; ++k)
		L.write(k, 7 * k);
	return nullptr;
}

void *reader(void *arg)
{
	long id = (long)arg;
	for (int k = 0; k < MO_K; ++k) {
		int a = -1, b = -1;
		got[id][k] = -1;
		if (L.try_read(a, b)) {
			assert(b == 7 * a && "torn read: the pair mixes two different writes");
			assert(a >= MO_PRE && a <= MO_PRE + MO_W && "read a value that is too old or never written");
			got[id][k] = a;
		}
	}
	return nullptr;
}

int main()
{
	int a = -1, b = -1;
	// solo prologue: initial state is (0, 0), writes are visible to their own thread
	assert(L.try_read(a, b) && a == 0 && b == 0 && "uncontended read of the initial value must succeed");
	for (int k = 1; k <= MO_PRE; ++k)
		L.write(k, 7 * k);
	assert(L.try_read(a, b) && a == MO_PRE && b == 7 * MO_PRE && "uncontended read must see the last write");

	pthread_t w, r[MO_R];
	pthread_create(&w, nullptr, writer, nullptr);
	for (long i = 0; i < MO_R; ++i)
		pthread_create(&r[i], nullptr, reader, (void *)i);
	pthread_join(w, nullptr);
	for (int i = 0; i < MO_R; ++i)
		pthread_join(r[i], nullptr);

	for (int i = 0; i < MO_R; ++i) {
		int last = -1;
		for (int k = 0; k < MO_K; ++k)
			if (got[i][k] >= 0) {
				assert(got[i][k] >= last && "a reader went back in time");
				last = got[i][k];
			}
	}
	assert(L.try_read(a, b) && a == MO_PRE + MO_W && b == 7 * a && "after join, a read must succeed and see the last write");
	return 0;
}
