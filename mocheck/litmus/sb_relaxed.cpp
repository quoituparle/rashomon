// expect: FAIL assert
// why: store buffering. With relaxed accesses both threads may read 0 (allowed even on x86).
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0}, y{0};
int r1, r2;
void *t1(void *) { x.store(1, std::memory_order_relaxed); r1 = y.load(std::memory_order_relaxed); return nullptr; }
void *t2(void *) { y.store(1, std::memory_order_relaxed); r2 = x.load(std::memory_order_relaxed); return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(!(r1 == 0 && r2 == 0) && "SB weak outcome");
	return 0;
}
