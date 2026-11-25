// expect: FAIL assert
// why: increment as separate load + store (even seq_cst) can lose an update. Control for
//      rmw_atomic. The two threads add different amounts so their stores never write the same
//      value (see same_value_writes.cpp for why that matters to GenMC).
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> c{0};
void *t(void *arg) {
	int v = c.load(std::memory_order_seq_cst);
	c.store(v + (int)(long)arg, std::memory_order_seq_cst);
	return nullptr;
}
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t, (void *)1);
	pthread_create(&b, nullptr, t, (void *)2);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(c.load(std::memory_order_relaxed) == 3 && "lost update");
	return 0;
}
