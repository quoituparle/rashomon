// expect: PASS
// known-wrong: genmc: stops on "Unordered writes" (treated as an error by its in-place revisiting): INCONCLUSIVE, a false alarm, never a false PASS
// why: two threads store the same value to one location, unordered. Correct, race-free code.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0};
void *t(void *) { x.store(1, std::memory_order_relaxed); return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_create(&b, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(x.load(std::memory_order_relaxed) == 1 && "value");
	return 0;
}
