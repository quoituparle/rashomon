// expect: PASS
// liveness: genmc
// why: correct spin-wait handoff (release flag, acquire spin): no false livelock, no race.
#include <atomic>
#include <cassert>
#include <pthread.h>
int data;
std::atomic<int> flag{0};
void *producer(void *) { data = 1; flag.store(1, std::memory_order_release); return nullptr; }
void *consumer(void *) {
	while (flag.load(std::memory_order_acquire) == 0) {
	}
	assert(data == 1 && "handoff lost the data");
	return nullptr;
}
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, consumer, nullptr);
	pthread_create(&b, nullptr, producer, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	return 0;
}
