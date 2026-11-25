// Tool-owned harness. The implementation under test cannot change anything here.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "rashomon_impl.hpp"

static_assert(MO_N >= MO_CAP + 2, "scenario rule: the queue must fill and wrap");

Queue<MO_CAP> q;

void *producer(void *)
{
	for (int i = 1; i <= MO_N; ++i)
		while (!q.push(i)) {
		}
	return nullptr;
}

void *consumer(void *)
{
	for (int i = 1; i <= MO_N; ++i) {
		int v = 0;
		while (!q.pop(v)) {
		}
		assert(v == i && "FIFO violated: value lost, duplicated or stale");
	}
	return nullptr;
}

int main()
{
	// solo prologue: exact capacity, failure paths return, FIFO; leaves the
	// queue empty with non-zero indices for the concurrent part.
	int v = 0;
	assert(!q.pop(v) && "pop on an empty queue must fail");
	for (int i = 0; i < MO_CAP; ++i)
		assert(q.push(100 + i) && "push below capacity must succeed");
	assert(!q.push(999) && "push on a full queue must fail");
	for (int i = 0; i < MO_CAP; ++i)
		assert(q.pop(v) && v == 100 + i && "solo FIFO violated");
	assert(!q.pop(v) && "pop on an empty queue must fail");

	pthread_t p, c;
	pthread_create(&p, nullptr, producer, nullptr);
	pthread_create(&c, nullptr, consumer, nullptr);
	pthread_join(p, nullptr);
	pthread_join(c, nullptr);
	return 0;
}
