#ifndef LIBUCD_INT24_H
#define LIBUCD_INT24_H 1

#include <inttypes.h>

typedef uint8_t int24[3];
typedef uint8_t uint24[3];

static inline uint32_t getuint24(const uint8_t *p)
{
  return (uint32_t)p[0] +
    ((uint32_t)p[1] << 8) +
    ((uint32_t)p[2] << 16);
}

static inline int32_t getint24(const uint8_t *p)
{
  return (int32_t)p[0] +
    ((int32_t)p[1] << 8) +
    ((int32_t)(int8_t)p[2] << 16);
}

#endif /* LIBUCD_INT24_H */
