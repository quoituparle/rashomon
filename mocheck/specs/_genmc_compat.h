// Tool-owned. Injected (-include) only into GenMC compilations.
// GenMC ships a minimal <pthread.h>; libstdc++'s threading layer (pulled in by
// <memory>, <functional>, ...) needs more of the POSIX API to *compile*. Code under
// test never calls these: they are declarations only, so any actual call would fail
// loudly at verification time instead of being silently modelled.
#ifndef MOCHECK_GENMC_COMPAT_H
#define MOCHECK_GENMC_COMPAT_H
#include <pthread.h>
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef int pthread_once_t;
typedef unsigned int pthread_key_t;
typedef struct { int __mocheck_unused; } pthread_cond_t;
typedef struct { int __mocheck_unused; } pthread_condattr_t;
#ifndef PTHREAD_ONCE_INIT
#define PTHREAD_ONCE_INIT 0
#endif
#ifndef PTHREAD_COND_INITIALIZER
#define PTHREAD_COND_INITIALIZER {0}
#endif
#ifndef PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_RECURSIVE 1
#endif
int sched_yield(void);
int rand(void);
void srand(unsigned);
int pthread_once(pthread_once_t *, void (*)(void));
int pthread_key_create(pthread_key_t *, void (*)(void *));
int pthread_key_delete(pthread_key_t);
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_equal(pthread_t, pthread_t);
int pthread_detach(pthread_t);
int pthread_mutexattr_init(pthread_mutexattr_t *);
int pthread_mutexattr_settype(pthread_mutexattr_t *, int);
int pthread_mutexattr_destroy(pthread_mutexattr_t *);
int pthread_mutex_timedlock(pthread_mutex_t *, const struct timespec *);
int pthread_cond_init(pthread_cond_t *, const pthread_condattr_t *);
int pthread_cond_destroy(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_broadcast(pthread_cond_t *);
#ifdef __cplusplus
}
namespace std { using ::rand; using ::srand; } // <algorithm> names std::rand
// Normally provided by the C runtime; needed when a global has a non-trivial destructor.
extern "C" __attribute__((weak)) void *__dso_handle = &__dso_handle;
// Exit-time destructors of globals are not run under GenMC: every thread has been
// joined by then, so they cannot take part in any interleaving being checked.
extern "C" __attribute__((weak)) int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
// GenMC has no C++ runtime, so exceptions cannot be modelled. Under GenMC only, the
// judge rewrites `throw expr;` in the implementation into a call to this function,
// which is declared but deliberately never defined: if a scenario ever reached a
// throw, verification would stop with "unknown external function" (INCONCLUSIVE),
// never silently continue. The native ASan/UBSan pass compiles the original code.
[[noreturn]] void __mocheck_throw_unreachable();
// Over-aligned allocation (e.g. cache-line-aligned slots): route the align_val_t
// overloads to GenMC's own aligned_alloc (the C++ path is not wired up in GenMC).
// Libraries may check the alignment they get, so it must really be honoured.
#include <new>
#include <stdlib.h>
namespace std { using ::malloc; using ::free; } // libraries spell std::malloc / std::free
static inline void *mocheck_aligned(std::size_t n, std::align_val_t al) {
	std::size_t a = static_cast<std::size_t>(al);
	std::size_t sz = n ? ((n + a - 1) / a) * a : a;
	return aligned_alloc(a, sz);
}
void *operator new(std::size_t n, std::align_val_t al) { return mocheck_aligned(n, al); }
void *operator new[](std::size_t n, std::align_val_t al) { return mocheck_aligned(n, al); }
void operator delete(void *p, std::align_val_t) noexcept { free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { free(p); }
// Sized deallocation (C++14): clang emits `delete p` on a complete class type as a call to
// operator delete(void*, size_t), which GenMC does not know ("unknown external function
// _ZdlPvm"). Found by litmus/global_ctor_ok.cpp.
void operator delete(void *p, std::size_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t) noexcept { free(p); }
#endif
#endif
