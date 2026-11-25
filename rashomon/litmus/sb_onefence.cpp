// expect: FAIL assert
// why: store buffering with a seq_cst fence in only ONE thread: the other thread's store and
//      load can still reorder, so the weak outcome stays allowed. Catches a model that treats
//      a single fence as globally ordering.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0}, y{0};
int r1, r2;
void *t1(void *) {
	x.store(1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	r1 = y.load(std::memory_order_relaxed);
	return nullptr;
}
void *t2(void *) {
	y.store(1, std::memory_order_relaxed);
	r2 = x.load(std::memory_order_relaxed);
	return nullptr;
}
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(!(r1 == 0 && r2 == 0) && "SB weak outcome");
	return 0;
}
