// expect: FAIL assert
// why: message passing with a relaxed flag: seeing the flag does not imply seeing the data.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> data{0}, flag{0};
void *t1(void *) {
	data.store(1, std::memory_order_relaxed);
	flag.store(1, std::memory_order_relaxed);
	return nullptr;
}
void *t2(void *) {
	if (flag.load(std::memory_order_relaxed) == 1)
		assert(data.load(std::memory_order_relaxed) == 1 && "MP: stale data after flag");
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
