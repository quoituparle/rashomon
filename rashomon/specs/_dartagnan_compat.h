// Tool-owned. Injected (-include) only into Dartagnan (second judge) compilations.
// Dartagnan 4.5.0 models malloc / aligned_alloc / free but not the C++ allocation
// functions: a call to operator new aborts parsing ("Register type mismatch"). These are
// the standard replaceable global allocation functions, defined on top of what it models.
// Like plain `new`, malloc'd memory is not zeroed in Dartagnan's model, so harnesses that
// rely on non-zeroed heap objects (ebr) keep their meaning. Allocation never fails here,
// the same assumption GenMC makes.
#ifndef RASHOMON_DARTAGNAN_COMPAT_H
#define RASHOMON_DARTAGNAN_COMPAT_H
#ifdef __cplusplus
#include <cstddef>
#include <new>
#include <stdlib.h>
static inline void *rashomon_dat_alloc(std::size_t n) { return malloc(n ? n : 1); }
static inline void *rashomon_dat_aligned(std::size_t n, std::align_val_t al) {
	std::size_t a = static_cast<std::size_t>(al);
	std::size_t sz = n ? ((n + a - 1) / a) * a : a;
	return aligned_alloc(a, sz);
}
void *operator new(std::size_t n) { return rashomon_dat_alloc(n); }
void *operator new[](std::size_t n) { return rashomon_dat_alloc(n); }
void *operator new(std::size_t n, std::align_val_t al) { return rashomon_dat_aligned(n, al); }
void *operator new[](std::size_t n, std::align_val_t al) { return rashomon_dat_aligned(n, al); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, std::size_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t) noexcept { free(p); }
void operator delete(void *p, std::align_val_t) noexcept { free(p); }
void operator delete[](void *p, std::align_val_t) noexcept { free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { free(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { free(p); }
#endif
#endif
