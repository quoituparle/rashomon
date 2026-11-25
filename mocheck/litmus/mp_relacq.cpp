// expect: PASS
// why: message passing, release store / acquire load of the flag: plain data is published, no race.
#include <atomic>
#include <cassert>
#include <pthread.h>
int data;
std::atomic<int> flag{0};
void *t1(void *) {
	data = 1;
	flag.store(1, std::memory_order_release);
	return nullptr;
}
void *t2(void *) {
	if (flag.load(std::memory_order_acquire) == 1)
		assert(data == 1 && "MP: stale data after flag");
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
