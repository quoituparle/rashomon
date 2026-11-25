// rashomon GenMC shim: declarations only. GenMC's runtime has no POSIX semaphores and the system
// header drags in glibc pthread types that clash with GenMC's. Code paths that actually CALL these
// cannot be model-checked (GenMC would report an unresolved call), which is the honest outcome.
#pragma once
#include <time.h>
extern "C" {
typedef struct { long __rashomon_opaque; } sem_t;
int sem_init(sem_t*, int, unsigned);
int sem_destroy(sem_t*);
int sem_wait(sem_t*);
int sem_trywait(sem_t*);
int sem_timedwait(sem_t*, const struct timespec*);
int sem_post(sem_t*);
}
