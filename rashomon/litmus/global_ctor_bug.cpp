// expect: FAIL assert
// why: the bug is reachable only because a global's dynamic initializer runs before main.
//      A checker that skips @llvm.global_ctors starts from limit == 0 and wrongly PASSes.
#include <atomic>
#include <cassert>
#include <pthread.h>
static int setup() { return 2; }
int limit = setup(); // dynamic initialization
std::atomic<int> x{0};
void *t(void *) { x.store(limit, std::memory_order_relaxed); return nullptr; }
int main() {
	pthread_t a;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	assert(x.load(std::memory_order_relaxed) != 2 && "reachable only if the global initializer ran");
	return 0;
}
