/**********************************************************
 * options_epilog.h -- epilogue for options.h
 *
 * Sanity-check and ensure defaults for various settings.
 * This needs to be a separate file, since we need to be
 * certain ./configure is not overwriting any of this.
 *
 * Resolution of defaults/conflicts in option values must be
 * done HERE and using AT MOST information from config.h.
 * Violation of this rule risks introducing circular
 * dependencies since options.h is usually included
 * early elsewhere.
 *
 * Options must NOT be re-#defined in any other file.
 * Violation of this rule risks introducing inconsistencies
 * since options.h may be included without the other file.
 */

#if defined(Options_Epilog_H) || !defined(Options_H)
#  error options_epilog.h must only be included from options.h
#else
#  define Options_Epilog_H 1


#ifdef BYTECODE_REDUCE_REF
#error Think carefully before enabling BYTECODE_REDUCE_REF.  This feature is still beta.  Comment out this line if you are sure.
#endif

/**********************************************************
 * You shouldn't need to change anything below this point.
 **********************************************************/

#define OPTION_DEFAULT -1

#define BQM_HW  2
#define BQM_32  3
#define BQM_64B 4
#define BQM_64  5
#ifndef BYTE_QUOTA_MODEL
#  define BYTE_QUOTA_MODEL BQM_64
#elif !(BQM_HW <= BYTE_QUOTA_MODEL && BYTE_QUOTA_MODEL <= BQM_64)
#  error "unknown BYTE_QUOTA_MODEL"
#endif

#define OBN_OFF		0
#define OBN_ON		1

#ifndef OUT_OF_BAND_PREFIX
#define OUT_OF_BAND_PREFIX ""
#endif
#ifndef OUT_OF_BAND_QUOTE_PREFIX
#define OUT_OF_BAND_QUOTE_PREFIX ""
#endif

#if DEFAULT_MAX_STACK_DEPTH < 3
#error No!
#endif

#if PATTERN_CACHE_SIZE < 1
#  error Illegal match() pattern cache size!
#endif

#define NP_SINGLE	1
#define NP_TCP		2
#define NP_LOCAL	3
#define NP_SESSION	4

#define NS_BSD		1
#define NS_SYSV		2

#define MP_SELECT	1
#define MP_POLL		2
#define MP_FAKE		3

#include "config.h"

#if NETWORK_PROTOCOL != NP_SINGLE  &&  !defined(MPLEX_STYLE)
#  if NETWORK_STYLE == NS_BSD
#    if HAVE_SELECT
#      define MPLEX_STYLE MP_SELECT
#    else
       #error You cannot use BSD sockets without having select()!
#    endif
#  else				/* NETWORK_STYLE == NS_SYSV */
#    if NETWORK_PROTOCOL == NP_LOCAL
#      if SELECT_WORKS_ON_FIFOS
#        define MPLEX_STYLE MP_SELECT
#      else
#        if POLL_WORKS_ON_FIFOS
#	   define MPLEX_STYLE MP_POLL
#	 else
#	   if FSTAT_WORKS_ON_FIFOS
#	     define MPLEX_STYLE MP_FAKE
#	   else
	     #error I need to be able to do a multiplexing wait on FIFOs!
#	   endif
#	 endif
#      endif
#    else			/* It's a TLI-based networking protocol */
#      if HAVE_POLL
#        define MPLEX_STYLE MP_POLL
#      else
         #error You cannot use TLI without having poll()!
#      endif
#    endif
#  endif
#endif

#if (NETWORK_PROTOCOL == NP_LOCAL || NETWORK_PROTOCOL == NP_SINGLE \
     || NETWORK_PROTOCOL == NP_SESSION) && defined(OUTBOUND_NETWORK)
#  error You cannot define "OUTBOUND_NETWORK" with that "NETWORK_PROTOCOL"
#endif

/* make sure OUTBOUND_NETWORK has a value;
   for backward compatibility, use 1 if none given */
#if defined(OUTBOUND_NETWORK) && (( 0 * OUTBOUND_NETWORK - 1 ) == 0)
#undef OUTBOUND_NETWORK
#define OUTBOUND_NETWORK 1
#endif


#if NETWORK_PROTOCOL != NP_LOCAL && NETWORK_PROTOCOL != NP_SINGLE \
    && NETWORK_PROTOCOL != NP_TCP && NETWORK_PROTOCOL != NP_SESSION
#  error Illegal value for "NETWORK_PROTOCOL"
#endif

#if NETWORK_STYLE != NS_BSD && NETWORK_STYLE != NS_SYSV && !(NETWORK_PROTOCOL == NP_SINGLE && !defined(NETWORK_STYLE))
#  error Illegal value for "NETWORK_STYLE"
#endif

#if defined(MPLEX_STYLE) 	\
    && MPLEX_STYLE != MP_SELECT \
    && MPLEX_STYLE != MP_POLL \
    && MPLEX_STYLE != MP_FAKE
#  error Illegal value for "MPLEX_STYLE"
#endif


#ifdef INT_TYPE_BITSIZE
#  if INT_TYPE_BITSIZE == 1
    /* Both
     *    --enable-def-INT_TYPE_BITSIZE=yes
     *    --disable-def-INT_TYPE_BITSIZE
     * should be ./configure errors because wtf.  But code
     * has not yet been written to do that, so cope here.
     */
#    undef INT_TYPE_BITSIZE
#  endif
#endif

#ifndef INT_TYPE_BITSIZE
#  if HAVE_INT64_T
#    define INT_TYPE_BITSIZE 64
#  else
#    define INT_TYPE_BITSIZE 32
#  endif
#endif

#if HAVE_INT64_T
/* everything is doable */
#elif INT_TYPE_BITSIZE == 64
#  error INT_TYPE_BITSIZE == 64 requires a platform that has 64-bit integers.
#elif HAVE_INT32_T
/* we can do 32 or 16 */
#elif INT_TYPE_BITSIZE == 32
#  error INT_TYPE_BITSIZE == 32 requires a platform that has 32-bit integers.
#endif

#if INT_TYPE_BITSIZE == 64
#  define NUM_MAX  INT64_MAX
#elif INT_TYPE_BITSIZE == 32
#  define NUM_MAX  INT32_MAX
#elif INT_TYPE_BITSIZE == 16
#  define NUM_MAX  INT16_MAX
#elif
#  error INT_TYPE_BITSIZE can only be 64, 32, or 16.
#endif

#if DEFAULT_MAX_LIST_CONCAT < MIN_LIST_CONCAT_LIMIT
#error DEFAULT_MAX_LIST_CONCAT < MIN_LIST_CONCAT_LIMIT ??
#endif
#if DEFAULT_MAX_STRING_CONCAT < MIN_STRING_CONCAT_LIMIT
#error DEFAULT_MAX_STRING_CONCAT < MIN_STRING_CONCAT_LIMIT ??
#endif

#if NUM_MAX < INTMAX_MAX
/* Options that are modifiable in-db must fit in a Num so we need an
 * additional upper bound if Num is not the widest possible integer
 * type.  This mostly only comes up in the INT16 world, but we may as
 * well be general.
 *
 * And hence we need NUM_MAX here rather than structures.h
 * because this file needs to be early, as explained above.
 *
 * The various subtractions of 1 are to nuke compiler
 * warnings about branches always going the same way,
 * which we might normally be concerned about but in
 * these cases, no.
 */
#  if DEFAULT_MAX_LIST_CONCAT   >= NUM_MAX
#    undef  DEFAULT_MAX_LIST_CONCAT
#    define DEFAULT_MAX_LIST_CONCAT	(NUM_MAX - 1)
#  endif
#  if DEFAULT_MAX_STRING_CONCAT >= NUM_MAX
#    undef  DEFAULT_MAX_STRING_CONCAT
#    define DEFAULT_MAX_STRING_CONCAT	(NUM_MAX - 1)
#  endif
#  if MAX_QUEUED_OUTPUT >= NUM_MAX
#    undef  MAX_QUEUED_OUTPUT
#    define MAX_QUEUED_OUTPUT	(NUM_MAX - 1)
#  endif
#  if MAX_QUEUED_INPUT  >= NUM_MAX
#    undef  MAX_QUEUED_INPUT
#    define MAX_QUEUED_INPUT	(NUM_MAX - 1)
#  endif
#endif


#define FT_FLOAT  2
#define FT_DOUBLE 3
#define FT_LONG   4
#define FT_QUAD   5

#ifndef FLOATING_TYPE
#  define FLOATING_TYPE  FT_DOUBLE
#elif FLOATING_TYPE==1
#  undef FLOATING_TYPE
#  define FLOATING_TYPE  FT_DOUBLE
#elif FLOATING_TYPE <= 0 || FT_QUAD < FLOATING_TYPE
#  error "unknown FLOATING_TYPE"
#endif

#if (( 0 * BOXED_FLOATS - 1 ) == 0)
#  undef BOXED_FLOATS
#  define BOXED_FLOATS 1
#elif BOXED_FLOATS == OPTION_DEFAULT
#  undef BOXED_FLOATS
#  if FLOATING_TYPE > FT_DOUBLE
#    define BOXED_FLOATS 1
#  endif
#endif

#if BOXED_FLOATS
#  define FLOATS_ARE_BOXED 1
#elif defined(FLOATS_ARE_BOXED)
#  error "do not do that."
#endif
/* Only FLOATS_ARE_BOXED should be referenced from here on. */

#if (( 0 * BQM_BOXED_FLOATS - 1 ) == 0)
#  undef    BQM_BOXED_FLOATS
#  define   BQM_BOXED_FLOATS   1
#elif BQM_BOXED_FLOATS == OPTION_DEFAULT
#  undef    BQM_BOXED_FLOATS
#  if BYTE_QUOTA_MODEL == BQM_64
     /* the only abstract model without boxed floats */
#  elif BYTE_QUOTA_MODEL != BQM_HW || FLOATS_ARE_BOXED
#    define BQM_BOXED_FLOATS   1
#  endif
#endif


#if UNICODE_NUMBERS && !UNICODE_STRINGS
#  error "UNICODE_NUMBERS requires --enable-unicode"
#endif
#if UNICODE_IDENTIFIERS && !UNICODE_STRINGS
#  error "UNICODE_IDENTIFIERS requires --enable-unicode"
#endif


#if defined(WAIF_DICT) && !defined(WAIF_CORE)
#  error "WAIF_DICT requires waif support (--enable-waifs)"
#endif

#if (( 0 * BQM_INCLUDES_WAIFS - 1 ) == 0)
#  undef    BQM_INCLUDES_WAIFS
#  define   BQM_INCLUDES_WAIFS 1
#elif BQM_INCLUDES_WAIFS == OPTION_DEFAULT
#  undef    BQM_INCLUDES_WAIFS
#  if WAIF_CORE
#    define BQM_INCLUDES_WAIFS 1
#  endif
#endif
#if BQM_INCLUDES_WAIFS
#  define BQM_WAIF_CORE_IFELSE(then,else)  then
#else
#  define BQM_WAIF_CORE_IFELSE(then,else)  else
#endif

/* For temporary fake implementation of pragmas that will go away: */
#define PG_ALL             -1
#define PG_FLOATINT_INEQ  0x1
#define PG_FLOATINT_EQ    0x2
#define PG_TONUM_RAISE    0x4
#define PG_TOFLOAT_EFLOAT 0x8

#define PRAGMA_ON(WHICH) ((PRAGMAS)&PG_##WHICH)
#define PRAGMA_OFF(WHICH) ((~(PRAGMAS))&PG_##WHICH)

#endif		/* !Options_Epilog_H */
