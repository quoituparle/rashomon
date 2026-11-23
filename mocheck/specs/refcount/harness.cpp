// Tool-owned harness. The implementation under test cannot change anything here.
//
// An object is shared by main and MO_T worker threads. Main takes one reference
// per worker and hands it over. Each worker writes its own slot of the object's
// (plain, non-atomic) data while holding a reference, optionally clones and drops
// extra references (MO_K times), then releases. Whoever's release reports "last"
// destroys the object: it must see every write made before any release, and then
// "frees" the data. Any access after the free is a data race or an assertion.
//
// MO_OVER (saturating spec only): main takes MO_EXTRA more references before the
// workers start, pushing the count past 2^MO_BITS - 1. The counter must saturate
// and the object must be leaked, never freed early.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "mocheck_impl.hpp"

#ifndef MO_OVER
#define MO_OVER 0
#endif
#ifndef MO_EXTRA
#define MO_EXTRA 0
#endif

RefCount<MO_BITS> rc; // starts at 1: main's own reference
int data[MO_T];
int freed = 0;
int was_last[MO_T + 1];

void destroy()
{
	for (int i = 0; i < MO_T; ++i)
		assert(data[i] == MO_K + 1 && "destructor does not see a write made before a release");
	for (int i = 0; i < MO_T; ++i)
		data[i] = -1; // free
	freed = freed + 1;
}

void *worker(void *arg)
{
	long t = (long)arg;
	for (int k = 0; k < MO_K; ++k) {
		rc.acquire(); // clone a reference while holding one
		data[t] = data[t] + 1;
		if (rc.release()) { // can never be the last: this thread still holds one
			was_last[t] = 1;
			destroy();
		}
	}
	data[t] = data[t] + 1;
	if (rc.release()) {
		was_last[t] = 1;
		destroy();
	}
	return nullptr;
}

int main()
{
	// solo prologue: an extra reference taken and dropped is never the last one
	rc.acquire();
	assert(!rc.release() && "releasing a non-last reference reported 'last'");

	for (int t = 0; t < MO_T; ++t)
		rc.acquire(); // one reference per worker, handed over at creation
	for (int e = 0; e < MO_EXTRA; ++e)
		rc.acquire(); // overflow scenario: references main never gives back

	pthread_t th[MO_T];
	for (long t = 0; t < MO_T; ++t)
		pthread_create(&th[t], nullptr, worker, (void *)t);
	if (rc.release()) { // main drops its own reference
		was_last[MO_T] = 1;
		destroy();
	}
	for (int t = 0; t < MO_T; ++t)
		pthread_join(th[t], nullptr);

	int lasts = 0;
	for (int t = 0; t <= MO_T; ++t)
		lasts += was_last[t];
	if (MO_OVER) {
		assert(lasts == 0 && freed == 0 && "after an overflow the object must be leaked, never freed");
	} else if (MO_EXTRA == 0) {
		assert(lasts == 1 && freed == 1 && "exactly one release must report 'last'");
	}
	return 0;
}
