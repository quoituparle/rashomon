// expect: PASS
// why: load buffering (each thread reads the other's later write). RC11, the model rashomon
//      promises, forbids it (po ∪ rf acyclic), like every real compiler/CPU pair. Note: ISO C++
//      technically allows it for relaxed atomics (the out-of-thin-air gap), so this is a point
//      where rashomon's model is deliberately STRONGER than the standard.
#include <atomic>
#include <cassert>
#include <pthread.h>
std::atomic<int> x{0}, y{0};
int r1, r2;
void *t1(void *) { r1 = x.load(std::memory_order_relaxed); y.store(1, std::memory_order_relaxed); return nullptr; }
void *t2(void *) { r2 = y.load(std::memory_order_relaxed); x.store(1, std::memory_order_relaxed); return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	assert(!(r1 == 1 && r2 == 1) && "LB outcome");
	return 0;
}
