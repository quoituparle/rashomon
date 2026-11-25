// expect: PASS
// why: an empty-template asm with an in/out operand is a compiler barrier that emits nothing:
//      the value is unchanged. GenMC alone evaluates the output as 0 (false FAIL); Dartagnan
//      alone fails to parse it. mocheck's staging rewrite erases it.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> out{0};
void *t(void *) {
	int v = 5;
	asm("" : "+r"(v));
	out.store(v, std::memory_order_relaxed);
	return nullptr;
}
int main() {
	pthread_t a;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	assert(out.load(std::memory_order_relaxed) == 5 && "asm barrier changed the value");
	return 0;
}
