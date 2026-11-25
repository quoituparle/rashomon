// expect: FAIL race
// why: writing one past the end of a member array is UB; here it lands on the neighbouring
//      field, which another thread writes concurrently. Both judges see the aliasing and
//      report a race. (GenMC's blind spot in fl_oob.hpp is narrower: an out-of-bounds access
//      to an array of atomics through a member call, see README "Known blind spots".)
#include <atomic>
#include <pthread.h>
struct S {
	int a[2];
	int b;
};
S s;
std::atomic<int> idx{2};
void *t1(void *) {
	int i = idx.load(std::memory_order_relaxed);
	s.a[i] = 1; // i == 2: out of bounds
	return nullptr;
}
void *t2(void *) { s.b = 2; return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	return 0;
}
