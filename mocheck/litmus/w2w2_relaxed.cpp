// expect: FAIL assert
// why: 2+2W. With relaxed (or release) stores, the per-location orders of x and y need not
//      agree with program order, so both first stores can end up last.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0}, y{0};
void *t1(void *) { x.store(1, std::memory_order_relaxed); y.store(2, std::memory_order_relaxed); return nullptr; }
void *t2(void *) { y.store(1, std::memory_order_relaxed); x.store(2, std::memory_order_relaxed); return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(!(x.load(std::memory_order_relaxed) == 1 && y.load(std::memory_order_relaxed) == 1) && "2+2W outcome");
	return 0;
}
