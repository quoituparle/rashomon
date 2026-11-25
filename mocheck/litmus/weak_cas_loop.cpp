// expect: PASS
// why: increment through a compare_exchange_weak retry loop never loses an update (spurious
//      failures only retry). Exercises the weak->strong staging rewrite.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> c{0};
void *t(void *) {
	int old = c.load(std::memory_order_relaxed);
	while (!c.compare_exchange_weak(old, old + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
	}
	return nullptr;
}
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_create(&b, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(c.load(std::memory_order_relaxed) == 2 && "lost update");
	return 0;
}
