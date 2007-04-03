#ifndef LIBUCD_COMPILER_H
#define LIBUCD_COMPILER_H

#if defined(__GNUC__)

#define ALIGNED(x) __attribute__((aligned(x)))
#define noreturn void __attribute__((noreturn))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#else

#define ALIGNED(x)
#define noreturn void
#define likely(x) (!!(x))
#define unlikely(x) (!!(x))

#endif

#endif /* LIBUCD_COMPILER_H */
