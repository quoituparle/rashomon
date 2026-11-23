// Tool-owned harness. The implementation under test cannot change anything here.
// MO_N worker threads each write their own plain slot, then count_down();
// MO_WAITERS threads wait() and then read every slot. A waiter must not get past
// wait() before all N count_downs, and must see everything written before them.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "mocheck_impl.hpp"

Latch *latch;
int slot[MO_N];

void *worker(void *arg)
{
	long i = (long)arg;
	slot[i] = 10 + (int)i;
	latch->count_down();
	return nullptr;
}

void *waiter(void *)
{
	latch->wait();
	for (int i = 0; i < MO_N; ++i)
		assert(slot[i] == 10 + i && "wait() returned before every count_down's writes were visible");
	return nullptr;
}

int main()
{
	latch = new Latch(MO_N);
	pthread_t w[MO_N], r[MO_WAITERS];
	for (long i = 0; i < MO_WAITERS; ++i)
		pthread_create(&r[i], nullptr, waiter, nullptr);
	for (long i = 0; i < MO_N; ++i)
		pthread_create(&w[i], nullptr, worker, (void *)i);
	for (int i = 0; i < MO_N; ++i)
		pthread_join(w[i], nullptr);
	for (int i = 0; i < MO_WAITERS; ++i)
		pthread_join(r[i], nullptr);
	latch->wait(); // already open: must return at once
	return 0;
}
