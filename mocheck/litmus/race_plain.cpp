// expect: FAIL race
// why: two threads write the same plain int with no synchronization.
#include <pthread.h>
int x;
void *t1(void *) { x = 1; return nullptr; }
void *t2(void *) { x = 2; return nullptr; }
int main() {
	pthread_t a, b;
	pthread_create(&a, nullptr, t1, nullptr);
	pthread_create(&b, nullptr, t2, nullptr);
	pthread_join(a, nullptr);
	pthread_join(b, nullptr);
	return 0;
}
