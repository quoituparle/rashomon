// expect: PASS
// why: message passing with relaxed accesses, release fence before the flag store and acquire
//      fence after the flag load: fence-to-fence synchronization publishes the plain data.
#include <atomic>
#include <cassert>
#include <pthread.h>
int data;
std::atomic<int> flag{0};
void *t1(void *) {
	data = 1;
	std::atomic_thread_fence(std::memory_order_release);
	flag.store(1, std::memory_order_relaxed);
	return nullptr;
}
void *t2(void *) {
	if (flag.load(std::memory_order_relaxed) == 1) {
		std::atomic_thread_fence(std::memory_order_acquire);
		assert(data == 1 && "MP: stale data after flag");
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
