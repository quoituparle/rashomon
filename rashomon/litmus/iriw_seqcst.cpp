// expect: PASS
// why: IRIW with seq_cst everywhere: all threads agree on a single order of the two writes.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0}, y{0};
int r1, r2, r3, r4;
void *w1(void *) { x.store(1, std::memory_order_seq_cst); return nullptr; }
void *w2(void *) { y.store(1, std::memory_order_seq_cst); return nullptr; }
void *rd1(void *) { r1 = x.load(std::memory_order_seq_cst); r2 = y.load(std::memory_order_seq_cst); return nullptr; }
void *rd2(void *) { r3 = y.load(std::memory_order_seq_cst); r4 = x.load(std::memory_order_seq_cst); return nullptr; }
int main() {
	pthread_t a, b, c, d;
	pthread_create(&a, nullptr, w1, nullptr);
	pthread_create(&b, nullptr, w2, nullptr);
	pthread_create(&c, nullptr, rd1, nullptr);
	pthread_create(&d, nullptr, rd2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	pthread_join(c, nullptr);
	pthread_join(d, nullptr);
	assert(!(r1 == 1 && r2 == 0 && r3 == 1 && r4 == 0) && "IRIW: readers disagree on write order");
	return 0;
}
