// Tool-owned harness. The implementation under test cannot change anything here.
//
// Every thread logs its own operations (argument + result). After all threads
// are joined, the main thread drains the queue, then searches for ONE total
// order of all logged operations that
//   (1) respects each thread's program order, and
//   (2) replays correctly on a sequential bounded FIFO of capacity CAP,
//       where push may fail only when full and pop may fail only when empty
//       (unless MO_SPURIOUS_FAIL: then failed operations are unconstrained),
// and after which the sequential FIFO holds exactly what was drained.
// If no such order exists, the implementation is wrong.
#include <pthread.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include "mocheck_impl.hpp"

#ifndef MO_SPURIOUS_FAIL
#define MO_SPURIOUS_FAIL 0 // 1: a failed push/pop during contention needs no justification
#endif

#define MO_THREADS (MO_P + MO_C)
#define MO_MAXOPS (MO_PK > MO_CK ? MO_PK : MO_CK)
#define MO_MAXVALS (MO_P * MO_PK + 1)

Queue<MO_CAP> q;

struct Op {
	bool is_push;
	int val; // pushed value, or popped value (-1 if pop failed)
	bool ok;
};

Op hist[MO_THREADS][MO_MAXOPS];
int nops[MO_THREADS];

void *producer(void *arg)
{
	long id = (long)arg;
	for (int k = 0; k < MO_PK; ++k) {
		int v = (int)id * 10 + k + 1; // distinct values
		bool ok = q.push(v);
		hist[id][k] = Op{true, v, ok};
	}
	nops[id] = MO_PK;
	return nullptr;
}

void *consumer(void *arg)
{
	long id = (long)arg;
	for (int k = 0; k < MO_CK; ++k) {
		int v = -1;
		bool ok = q.pop(v);
		hist[id][k] = Op{false, ok ? v : -1, ok};
	}
	nops[id] = MO_CK;
	return nullptr;
}

// ---- sequential model + search ----
int drained[MO_MAXVALS];
int ndrained;
int mq[MO_MAXVALS + 1];
int mh, mt;
int pos[MO_THREADS];

bool search(int done, int total)
{
	if (done == total) {
		if (mt - mh != ndrained)
			return false;
		for (int i = 0; i < ndrained; ++i)
			if (mq[mh + i] != drained[i])
				return false;
		return true;
	}
	// With spurious failures allowed, a failed operation changes nothing and needs
	// nothing, so it can always be placed right now: no need to branch on it.
	if (MO_SPURIOUS_FAIL) {
		for (int t = 0; t < MO_THREADS; ++t) {
			if (pos[t] < nops[t] && !hist[t][pos[t]].ok) {
				++pos[t];
				bool r = search(done + 1, total);
				--pos[t];
				return r;
			}
		}
	}
	for (int t = 0; t < MO_THREADS; ++t) {
		if (pos[t] == nops[t])
			continue;
		Op o = hist[t][pos[t]];
		int size = mt - mh;
		if (o.is_push && o.ok) {
			if (size == MO_CAP)
				continue;
			mq[mt++] = o.val;
			++pos[t];
			if (search(done + 1, total))
				return true;
			--pos[t];
			--mt;
		} else if (o.is_push) {
			if (!MO_SPURIOUS_FAIL && size != MO_CAP)
				continue;
			++pos[t];
			if (search(done + 1, total))
				return true;
			--pos[t];
		} else if (o.ok) {
			if (size == 0 || mq[mh] != o.val)
				continue;
			++mh;
			++pos[t];
			if (search(done + 1, total))
				return true;
			--pos[t];
			--mh;
		} else {
			if (!MO_SPURIOUS_FAIL && size != 0)
				continue;
			++pos[t];
			if (search(done + 1, total))
				return true;
			--pos[t];
		}
	}
	return false;
}

int main()
{
	pthread_t th[MO_THREADS];
	for (long i = 0; i < MO_P; ++i)
		pthread_create(&th[i], nullptr, producer, (void *)i);
	for (long i = MO_P; i < MO_THREADS; ++i)
		pthread_create(&th[i], nullptr, consumer, (void *)i);
	for (int i = 0; i < MO_THREADS; ++i)
		pthread_join(th[i], nullptr);

	// drain (single-threaded, happens-after every logged operation)
	int v;
	while (ndrained < MO_MAXVALS && q.pop(v))
		drained[ndrained++] = v;

	int total = 0;
	for (int i = 0; i < MO_THREADS; ++i)
		total += nops[i];
	if (!search(0, total)) {
		// print the offending history so a counterexample is readable without the event graph
		for (int t = 0; t < MO_THREADS; ++t) {
			printf("MOCHECK| %s %d:", t < MO_P ? "producer" : "consumer", t);
			for (int k = 0; k < nops[t]; ++k) {
				Op o = hist[t][k];
				if (o.is_push)
					printf(" push(%d)%s", o.val, o.ok ? "" : "=full");
				else if (o.ok)
					printf(" pop->%d", o.val);
				else
					printf(" pop->empty");
			}
			printf("\n");
		}
		printf("MOCHECK| left in queue after join:");
		for (int i = 0; i < ndrained; ++i)
			printf(" %d", drained[i]);
		printf("\n");
		assert(0 && "history is not explainable by any sequential FIFO");
	}

	// solo phase on the now-empty queue: capacity is exactly CAP, failure
	// paths terminate, FIFO holds across the wrap-around.
	assert(!q.pop(v) && "pop on an empty queue must fail");
	for (int i = 0; i < MO_CAP; ++i)
		assert(q.push(100 + i) && "push below capacity must succeed");
	assert(!q.push(999) && "push on a full queue must fail");
	for (int i = 0; i < MO_CAP; ++i)
		assert(q.pop(v) && v == 100 + i && "solo FIFO violated");
	assert(!q.pop(v) && "pop on an empty queue must fail");
	return 0;
}
