// expect: PASS
// why: correct code whose shared object is built by a global initializer through `new` (the
//      ebr harness pattern). Skipping global constructors, or not modelling operator new,
//      gives a false FAIL here.
#include <atomic>
#include <cassert>
#include <pthread.h>
struct Box {
	std::atomic<int> v;
	int tag;
	Box() : v(7), tag(42) {}
};
Box &box = *new Box;
void *t(void *) {
	assert(box.v.load(std::memory_order_relaxed) == 7 && "constructor did not run");
	return nullptr;
}
int main() {
	pthread_t a;
	pthread_create(&a, nullptr, t, nullptr);
	pthread_join(a, nullptr);
	assert(box.tag == 42 && "constructor did not run");
	Box *p = new Box;
	assert(p->tag == 42 && "operator new + constructor");
	delete p;
	return 0;
}
