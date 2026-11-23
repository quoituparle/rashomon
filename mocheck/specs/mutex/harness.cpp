// Tool-owned harness. The implementation under test cannot change anything here.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "mocheck_impl.hpp"

Mutex m;
int counter = 0; // deliberately non-atomic: any overlap of critical sections is a data race

void *worker(void *)
{
	for (int k = 0; k < MO_K; ++k) {
		m.lock();
		counter = counter + 1;
		m.unlock();
	}
	return nullptr;
}

int main()
{
	// solo prologue: an uncontended lock/unlock pair must work and be repeatable
	m.lock();
	m.unlock();
	m.lock();
	m.unlock();

	pthread_t t[MO_T];
	for (int i = 0; i < MO_T; ++i)
		pthread_create(&t[i], nullptr, worker, nullptr);
	for (int i = 0; i < MO_T; ++i)
		pthread_join(t[i], nullptr);
	assert(counter == MO_T * MO_K && "lost update");
	return 0;
}
