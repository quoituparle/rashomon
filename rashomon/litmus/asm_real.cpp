// expect: INCONCLUSIVE
// why: a real asm instruction (here it changes the value). Neither checker can model it
//      faithfully, so rashomon must refuse to judge rather than guess.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> out{0};
void *t(void *) {
	int v = 5;
	asm volatile("add $1, %0" : "+r"(v));
	out.store(v, std::memory_order_relaxed);
	return nullptr;
}
int main() {
	pthread_t a;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	assert(out.load(std::memory_order_relaxed) == 6 && "asm add");
	return 0;
}
