// expect: FAIL race
// why: message passing with a relaxed flag and plain data: the data accesses race (UB in C++).
#include <atomic>
#include <pthread.h>
int data;
std::atomic<int> flag{0};
int seen;
void *t1(void *) {
	data = 1;
	flag.store(1, std::memory_order_relaxed);
	return nullptr;
}
void *t2(void *) {
	if (flag.load(std::memory_order_relaxed) == 1)
		seen = data;
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
