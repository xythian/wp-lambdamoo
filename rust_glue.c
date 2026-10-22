/*
 * rust_glue.c
 *
 * Linked only when some modules come from the Rust library in rust/
 * (make RUST_MODULES=...).  Refers to a symbol the library defines only
 * when it was built for the same Num size as this server, so a mismatch
 * is a link error instead of silently wrong integers.
 */

#include "structures.h"

#if INT_TYPE_BITSIZE == 64
extern const unsigned char moo_rs_num_bits_64;
const unsigned char *const moo_rs_num_check = &moo_rs_num_bits_64;
#elif INT_TYPE_BITSIZE == 32
extern const unsigned char moo_rs_num_bits_32;
const unsigned char *const moo_rs_num_check = &moo_rs_num_bits_32;
#else
#  error "the Rust modules support only 32- and 64-bit Num"
#endif
