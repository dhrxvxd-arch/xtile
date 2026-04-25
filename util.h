#if !defined(XTILE_UTIL_H)
#define XTILE_UTIL_H

#include <stddef.h>
#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define PRINTF_FMT(a, b)
#endif

_Noreturn void die(const char *fmt, ...) PRINTF_FMT(1, 2);
void *ecalloc(size_t nmemb, size_t size);

#endif