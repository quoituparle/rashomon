// expect: FAIL assert
// why: the bug only shows after 6 iterations of a loop whose trip count is read at run time.
//      A bounded checker must raise its bound until it finds it (Dartagnan: bound 8), not
//      report PASS at a smaller bound.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> n{6}, x{0};
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
	assert(x.load(std::memory_order_relaxed) != 6 && "reached only after 6 iterations");
	return 0;
}
