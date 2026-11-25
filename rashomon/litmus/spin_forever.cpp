// expect: FAIL liveness
// liveness: genmc
// why: a thread waits for a flag nobody ever sets: it is stuck forever.
#include <atomic>
#include <pthread.h>
std::atomic<int> flag{0};
void *t(void *) {
	while (flag.load(std::memory_order_acquire) == 0) {
	}
	return nullptr;
}
int main() {
	pthread_t a;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	return 0;
}
