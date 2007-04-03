/*
 * libucd_int.h
 *
 * Internal header file for libucd
 */

#ifndef LIBUCD_INT_H
#define LIBUCD_INT_H

#include <stdlib.h>
#include "ucd.h"
#include "int24.h"
#include "compiler.h"

#define UCS_CNT	0x110000
#define UCS_MAX (UCS_CNT-1)

extern const char _libucd_hangul_jamo_l[][4];
extern const char _libucd_hangul_jamo_v[][4];
extern const char _libucd_hangul_jamo_t[][4];

/* This structure is exactly 32 bytes long, nice and alignable. */
struct _libucd_property_array {
  int32_t  ucs;			/* Wasteful but fast (used in search) */
  uint8_t  general_category;
  uint8_t  script;
  uint8_t  numeric_value_num;
  uint8_t  numeric_value_den_exp; /* bit 7 = 1 if exponent */
  uint64_t flags_block;		/* Block index is byte 6, byte 7 free */
  int24    simple_uppercase;
  int24    simple_lowercase;
  int24    simple_titlecase;
  uint8_t  age;			/* (major << 3) + minor */
  uint8_t  canonical_combining_class;
  unsigned sentence_break	:4;
  unsigned grapheme_cluster_break :4;
  unsigned word_break		:3;
  unsigned joining_type  	:3;
  unsigned joining_group 	:6;
  unsigned east_asian_width     :3;
  unsigned hangul_syllable_type :3;
  unsigned line_break           :6;
  unsigned numeric_type         :2;
  unsigned bidi_class           :5;
  unsigned /* unused */         :1;
} ALIGNED(32);
extern const struct _libucd_property_array _libucd_property_array[];
extern const int _libucd_property_array_count;

struct _libucd_ucstoname_tab {
  int24    ucs;
  uint24   names_offset;
  uint16_t proparray_offset;
} ALIGNED(8);
extern const struct _libucd_ucstoname_tab _libucd_ucstoname_tab[];

struct _libucd_nametoucs_tab {
  int24	   ucs;
};
extern const struct _libucd_nametoucs_tab _libucd_nametoucs_tab[];

extern const unsigned char _libucd_names_list[];
extern const char * const _libucd_nameslist_dict[256];

/* This is a private data structure included in each ucd object */
#if defined(__i386__) || defined(__x86_64__)
# define HAVE_ATOMIC_CTR
#endif

struct libucd_private {
#if defined(HAVE_PTHREAD_H) && !defined(HAVE_ATOMIC_CTR)
  pthread_mutex_t mutex;
#endif
  volatile unsigned int usage_ctr;
};

const struct unicode_character_data *
_libucd_character_data_raw(int32_t ucs);

struct libucd_enum_names {
  const char *long_name;
  const char *short_name;
};

#endif
