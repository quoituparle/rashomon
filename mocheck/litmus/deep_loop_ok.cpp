// expect: PASS
// expect-dartagnan: INCONCLUSIVE
// why: correct code with a run-time loop of 20 iterations, beyond Dartagnan's largest bound
//      (16). GenMC explores it fully and PASSes; a bounded checker must say INCONCLUSIVE,
//      never PASS on a partial unrolling.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> n{20}, x{0};
void *t(void *) {
	int k = n.load(std::memory_order_relaxed);
	for (int i = 0; i < k; ++i)
		x.fetch_add(1, std::memory_order_relaxed);
	return nullptr;
}
int main() {
	pthread_t a;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	assert(x.load(std::memory_order_relaxed) == 20 && "wrong count");
	return 0;
}
