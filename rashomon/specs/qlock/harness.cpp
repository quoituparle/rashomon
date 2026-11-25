// Tool-owned harness. The implementation under test cannot change anything here.
// T threads each do K critical sections incrementing a plain int, each time with a
// fresh queue node on their own stack (as MCS-style locks are used).
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "rashomon_impl.hpp"

QLock m;
int counter = 0; // plain: overlapping critical sections are a data race

void *worker(void *)
{
	for (int k = 0; k < MO_K; ++k) {
		QLock::Node n;
		m.lock(n);
		counter = counter + 1;
		m.unlock(n);
	}
	return nullptr;
}

int main()
{
	{ // solo prologue: uncontended lock/unlock works and is repeatable
		QLock::Node a;
		m.lock(a);
		m.unlock(a);
		QLock::Node b;
		m.lock(b);
		m.unlock(b);
	}
	pthread_t t[MO_T];
	for (int i = 0; i < MO_T; ++i)
		pthread_create(&t[i], nullptr, worker, nullptr);
	for (int i = 0; i < MO_T; ++i)
		pthread_join(t[i], nullptr);
	assert(counter == MO_T * MO_K && "lost update");
	return 0;
}
