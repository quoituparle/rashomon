// expect: FAIL race
// why: C++20 (P0982R1): a later relaxed store by the SAME thread no longer continues the
//      release sequence. Reading that store with acquire does not synchronize, so the plain
//      data read races. A C++11-style model (stock Dat3M rc11.cat) wrongly PASSes this.
#include <atomic>
#include <pthread.h>
int data;
std::atomic<int> x{0};
int seen;
void *w(void *) {
	data = 1;
	x.store(1, std::memory_order_release);
	x.store(2, std::memory_order_relaxed);
	return nullptr;
}
void *r(void *) {
	if (x.load(std::memory_order_acquire) == 2)
		seen = data;
	return nullptr;
}
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, w, nullptr);
	pthread_create(&b, nullptr, r, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	return 0;
}
