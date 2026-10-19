/******************************************************************************
  Copyright (c) 1992, 1995, 1996 Xerox Corporation.  All rights reserved.
  Portions of this code were written by Stephen White, aka ghond.
  Use and copying of this software and preparation of derivative works based
  upon this software are permitted.  Any distribution of this software or
  derivative works must comply with all applicable United States export
  control laws.  This software is made available AS IS, and Xerox Corporation
  makes no warranty about the software, its performance or its conformity to
  any specification.  Any person obtaining a copy of this software is requested
  to send their name and post office or electronic mail address to:
    Pavel Curtis
    Xerox PARC
    3333 Coyote Hill Rd.
    Palo Alto, CA 94304
    Pavel@Xerox.Com
 *****************************************************************************/

#ifndef Utils_H
#define Utils_H 1

#include "config.h"

#include "my-stdio.h"

#include "execute.h"
#include "streams.h"

#undef MAX
#undef MIN
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))

#define Arraysize(x) (sizeof(x) / sizeof(*x))

extern int mystrcasecmp(const char *, const char *);
extern int mystrncasecmp(const char *, const char *, int);

extern int verbcasecmp(const char *verb, const char *word);

extern unsigned str_hash(const char *);

extern void complex_free_var(Var);
extern Var complex_var_ref(Var);
extern Var complex_var_dup(Var);
extern int var_refcount(Var);

static inline void
free_var(Var v)
{
    if (v.type & TYPE_COMPLEX_FLAG)
	complex_free_var(v);
}

static inline Var
var_ref(Var v)
{
    if (v.type & TYPE_COMPLEX_FLAG)
	return complex_var_ref(v);
    else
	return v;
}

static inline Var
var_dup(Var v)
{
    if (v.type & TYPE_COMPLEX_FLAG)
	return complex_var_dup(v);
    else
	return v;
}

extern int equality(Var lhs, Var rhs, int case_matters);
extern int is_true(Var v);

extern void stream_add_strsub(Stream *, const char *, const char *, const char *, int);
extern int strindex(const char *, const char *, int);
extern int strrindex(const char *, const char *, int);

extern Var get_system_property(const char *);
extern Objid get_system_object(const char *);

extern size_t value_bytes(Var);

/* Adds sizes, sticking at SIZE_MAX instead of wrapping. */
static inline size_t
size_add(size_t a, size_t b)
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

/* Converts a byte count to a MOO integer, sticking at the largest one. */
static inline Num
size_to_num(size_t n)
{
    return n > (size_t) NUM_MAX ? NUM_MAX : (Num) n;
}

/* Walking nested lists and waifs.
 *
 * A value can be nested far deeper than the C stack allows (x = {x}
 * repeated a million times), and lists can share sublists, so a value
 * built from a handful of lists can have exponentially many paths
 * through it (x = {x, x} repeated 60 times).  Code that visits every
 * element of a value therefore keeps its own stack of Walk_Frames
 * instead of recursing, and remembers the lists it has finished in a
 * Visit_Set so that it handles each shared sublist once.
 */
typedef struct {
    Var a;			/* the list or waif being walked */
    Var b;			/* its partner, when walking two values */
    Num i;			/* how many children have been visited */
    size_t aux;			/* running result for this frame */
} Walk_Frame;

typedef struct {
    Walk_Frame *frames;
    size_t depth, max;
} Walk_Stack;

/* Pushes a frame for A (and B) and returns it.  The stack's storage is
 * kept between walks; walk_release() frees it if it grew large, and
 * walk_free() frees it in any case.
 */
extern Walk_Frame *walk_push(Walk_Stack *, Var a, Var b, size_t aux);
extern void walk_release(Walk_Stack *);
extern void walk_free(Walk_Stack *);

/* Whether V is a list or waif, whose children a walk must visit. */
static inline int
walk_has_children(Var v)
{
    return v.type == TYPE_LIST
#ifdef WAIF_CORE
	|| v.type == TYPE_WAIF
#endif
	;
}

/* The elements of list V or the property values of waif V, in *COUNT. */
extern Var *walk_children(Var v, Num *count);

/* The identity of list or waif V, for a Visit_Set. */
static inline const void *
walk_id(Var v)
{
#ifdef WAIF_CORE
    if (v.type == TYPE_WAIF)
	return v.v.waif;
#endif
    return v.v.list;
}

typedef struct {
    const void *a, *b;		/* the key: one value's identity, or two */
    size_t x, y;		/* whatever the walk wants to remember */
} Visit;

typedef struct {
    Visit *slots;		/* open-addressed; a == 0 means empty */
    size_t size;		/* 0, or a power of 2 */
    size_t count;
} Visit_Set;

/* Number of lists a walk handles before a Visit_Set is worth its cost.
 * Below this, even a value full of shared sublists is cheap to walk.
 */
#define WALK_MEMO_AFTER 1024

extern Visit *visit_find(const Visit_Set *, const void *a, const void *b);
/* Returns the entry for (A, B), adding a zeroed one if there is none;
 * *ADDED (if not null) says whether it was added.
 */
extern Visit *visit_add(Visit_Set *, const void *a, const void *b,
			int *added);
extern void visit_set_free(Visit_Set *);

extern void stream_add_moobinary_from_raw_bytes(
    Stream *, const char *buffer, size_t buflen);
extern const char *moobinary_to_raw_bytes(const char *binary, size_t *rawlen);

/* Given a stream and a buffer of characters encoded as bytes using
 * fromcode, write the characters to the stream using tocode,
 * fromcode and tocode being encodings the iconv library
 * knows about.  Return nonzero if successful.
 */
extern int stream_add_recoded_chars(Stream *s,
				    const char *inbuf, size_t inbytesleft,
				    const char *fromcode,
				    const char *tocode);

#endif		/* !Utils_H */


/*
 * $Log$
 * Revision 2.4  1996/02/08  06:08:07  pavel
 * Moved become_number and compare_ints to number.h.  Updated copyright notice
 * for 1996.  Release 1.8.0beta1.
 *
 * Revision 2.3  1996/01/11  07:46:18  pavel
 * Added raw_bytes_to_binary() and binary_to_raw_bytes(), in support of binary
 * I/O facilities.  Release 1.8.0alpha5.
 *
 * Revision 2.2  1995/12/28  00:39:38  pavel
 * Removed declaration for private variable `cmap'.  Release 1.8.0alpha3.
 *
 * Revision 2.1  1995/12/11  08:09:56  pavel
 * Add `value_bytes()'.  Release 1.8.0alpha2.
 *
 * Revision 2.0  1995/11/30  04:56:31  pavel
 * New baseline version, corresponding to release 1.8.0alpha1.
 *
 * Revision 1.10  1992/10/23  23:03:47  pavel
 * Added copyright notice.
 *
 * Revision 1.9  1992/10/21  03:02:35  pavel
 * Converted to use new automatic configuration system.
 *
 * Revision 1.8  1992/10/17  20:57:30  pavel
 * Global rename of strdup->str_dup, strref->str_ref, vardup->var_dup, and
 * varref->var_ref.
 * Removed useless #ifdef of STRCASE.
 *
 * Revision 1.7  1992/09/08  21:55:12  pjames
 * Added `become_number()' (from bf_num.c).
 *
 * Revision 1.6  1992/09/03  16:23:16  pjames
 * Make cmap[] visible.
 *
 * Revision 1.5  1992/08/31  22:22:05  pjames
 * Changed some `char *'s to `const char *'
 *
 * Revision 1.4  1992/08/28  16:24:15  pjames
 * Added `varref()'.  Removed `copy_pi()'.
 *
 * Revision 1.3  1992/08/10  16:38:28  pjames
 * Added is_true (from execute.h) and *_activ_as_pi routines.
 *
 * Revision 1.2  1992/07/27  18:03:48  pjames
 * Changed name of ct_env to var_names
 *
 * Revision 1.1  1992/07/20  23:23:12  pavel
 * Initial RCS-controlled version.
 */
