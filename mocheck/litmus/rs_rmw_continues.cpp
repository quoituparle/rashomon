// expect: PASS
// why: release sequence continued by an RMW of another thread (still true in C++20): the
//      acquirer that reads the RMW's value synchronizes with the original release store.
#include <atomic>
#include <cassert>
#include <pthread.h>
int data;
std::atomic<int> x{0};
void *rel(void *) { data = 1; x.store(1, std::memory_order_release); return nullptr; }
void *rmw(void *) { x.fetch_add(1, std::memory_order_relaxed); return nullptr; }
void *acq(void *) {
	if (x.load(std::memory_order_acquire) == 2)
		assert(data == 1 && "release sequence through RMW broken");
	return nullptr;
}
int main() {
	pthread_t a, b, c;
	pthread_create(&a, nullptr, rel, nullptr);
	pthread_create(&b, nullptr, rmw, nullptr);
	pthread_create(&c, nullptr, acq, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	pthread_join(c, nullptr);
	return 0;
}
