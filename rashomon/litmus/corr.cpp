// expect: PASS
// why: read-read coherence. Even relaxed, two reads of one location in one thread never go
//      backwards in its modification order.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0};
int a, b;
void *w(void *) {
	x.store(1, std::memory_order_relaxed);
	x.store(2, std::memory_order_relaxed);
	return nullptr;
}
void *r(void *) {
	a = x.load(std::memory_order_relaxed);
	b = x.load(std::memory_order_relaxed);
	return nullptr;
}
int main() {
	pthread_t t1, t2;
	pthread_create(&t1, nullptr, w, nullptr);
	pthread_create(&t2, nullptr, r, nullptr);
	pthread_join(t1, nullptr);
	pthread_join(t2, nullptr);
	assert(!(a == 2 && b == 1) && "CoRR: read went backwards");
	assert(!(a > 0 && b == 0) && "CoRR: read went backwards");
	return 0;
}
