// Tool-owned harness. The implementation under test cannot change anything here.
// T threads race to call once.call(init); init writes plain data. Every call must
// return only after init has run, see everything init wrote, and init must run
// exactly once.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include "mocheck_impl.hpp"

Once once;
int config_a = 0, config_b = 0; // plain data written by init
int init_runs = 0;               // plain: two concurrent inits also race here

void init()
{
	init_runs = init_runs + 1;
	config_a = 42;
	config_b = 43;
}

void *worker(void *)
{
	for (int k = 0; k < MO_K; ++k) {
		once.call(init);
		assert(config_a == 42 && config_b == 43 && "call() returned before the initialisation was visible");
	}
	return nullptr;
}

int main()
{
	pthread_t t[MO_T];
	for (int i = 0; i < MO_T; ++i)
		pthread_create(&t[i], nullptr, worker, nullptr);
	for (int i = 0; i < MO_T; ++i)
		pthread_join(t[i], nullptr);
	once.call(init); // after everyone: still exactly once
	assert(init_runs == 1 && "init ran more than once (or never)");
	return 0;
}
