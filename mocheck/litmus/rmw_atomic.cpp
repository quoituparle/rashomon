// expect: PASS
// why: RMW atomicity: two relaxed fetch_adds never lose an update.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> c{0};
void *t(void *) { c.fetch_add(1, std::memory_order_relaxed); return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_create(&b, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(c.load(std::memory_order_relaxed) == 2 && "lost update");
	return 0;
}
