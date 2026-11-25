// expect: FAIL assert
// why: atomic_signal_fence orders only against a signal handler on the same thread; between
//      threads this is relaxed message passing, so stale data is allowed. Both GenMC and
//      Dartagnan treat it as a thread fence on their own; rashomon's staging rewrite fixes that.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> data{0}, flag{0};
void *t1(void *) {
	data.store(1, std::memory_order_relaxed);
	std::atomic_signal_fence(std::memory_order_release);
	flag.store(1, std::memory_order_relaxed);
	return nullptr;
}
void *t2(void *) {
	if (flag.load(std::memory_order_relaxed) == 1) {
		std::atomic_signal_fence(std::memory_order_acquire);
		assert(data.load(std::memory_order_relaxed) == 1 && "MP: stale data after flag");
	}
	return nullptr;
}
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	return 0;
}
