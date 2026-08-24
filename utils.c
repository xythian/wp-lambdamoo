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

#include "utils.h"

#include "options.h"

#include "my-ctype.h"
#include "my-stdio.h"
#include "my-string.h"
#include <iconv.h>
#include <errno.h>

#include "bound.h"
#include "db.h"
#include "db_io.h"
#include "exceptions.h"
#include "list.h"
#include "log.h"
#include "match.h"
#include "numbers.h"
#include "ref_count.h"
#include "server.h"
#include "storage.h"
#include "streams.h"
#include "structures.h"
#include "utf.h"
#include "waif.h"


/*
 * These versions of strcasecmp() and strncasecmp() depend on ASCII.
 * We implement them here because neither one is in the ANSI standard.
 */

static const char cmap[] =
"\000\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017"
"\020\021\022\023\024\025\026\027\030\031\032\033\034\035\036\037"
"\040\041\042\043\044\045\046\047\050\051\052\053\054\055\056\057"
"\060\061\062\063\064\065\066\067\070\071\072\073\074\075\076\077"
"\100\141\142\143\144\145\146\147\150\151\152\153\154\155\156\157"
"\160\161\162\163\164\165\166\167\170\171\172\133\134\135\136\137"
"\140\141\142\143\144\145\146\147\150\151\152\153\154\155\156\157"
"\160\161\162\163\164\165\166\167\170\171\172\173\174\175\176\177"
"\200\201\202\203\204\205\206\207\210\211\212\213\214\215\216\217"
"\220\221\222\223\224\225\226\227\230\231\232\233\234\235\236\237"
"\240\241\242\243\244\245\246\247\250\251\252\253\254\255\256\257"
"\260\261\262\263\264\265\266\267\270\271\272\273\274\275\276\277"
"\300\301\302\303\304\305\306\307\310\311\312\313\314\315\316\317"
"\320\321\322\323\324\325\326\327\330\331\332\333\334\335\336\337"
"\340\341\342\343\344\345\346\347\350\351\352\353\354\355\356\357"
"\360\361\362\363\364\365\366\367\370\371\372\373\374\375\376\377";

int
mystrcasecmp(const char *ss, const char *tt)
{
    register const unsigned char *s = (const unsigned char *) ss;
    register const unsigned char *t = (const unsigned char *) tt;

    if (s == t) {
	return 0;
    }
    while (cmap[*s] == cmap[*t++]) {
	if (!*s++)
	    return 0;
    }
    return (cmap[*s] - cmap[*--t]);
}

int
mystrncasecmp(const char *ss, const char *tt, int n)
{
    const unsigned char *s = (const unsigned char *) ss;
    const unsigned char *t = (const unsigned char *) tt;

    if (!n || ss == tt)
	return 0;
    while (cmap[*s] == cmap[*t++]) {
	if (!*s++ || !--n)
	    return 0;
    }
    return (cmap[*s] - cmap[*--t]);
}

int
verbcasecmp(const char *verb, const char *word)
{
    const unsigned char *w;
    const unsigned char *v = (const unsigned char *) verb;
    enum {
	none, inner, end
    } star;

    if (verb == word) {
	return 1;
    }
    while (*v) {
	w = (const unsigned char *) word;
	star = none;
	while (1) {
	    while (*v == '*') {
		v++;
		star = (!*v || *v == ' ') ? end : inner;
	    }
	    if (!*v || *v == ' ' || !*w || cmap[*w] != cmap[*v])
		break;
	    w++;
	    v++;
	}
	if (!*w ? (star != none || !*v || *v == ' ')
	    : (star == end))
	    return 1;
	while (*v && *v != ' ')
	    v++;
	while (*v == ' ')
	    v++;
    }
    return 0;
}

unsigned
str_hash(const char *s)
{
    register unsigned ans = 0;

    while (*s) {
	ans = (ans << 3) + (ans >> 28) + cmap[(unsigned char) *s++];
    }
    return ans;
}

/* Freeing, comparing and sizing values recurse while that is safe, and
 * switch to walking with an explicit stack once a value turns out to hold
 * more lists, or deeper ones, than these.
 */
#define SMALL_WALK_LISTS 256
#define SMALL_WALK_DEPTH 32

static void *
walk_grow(void *p, size_t size)
{
    return p ? myrealloc(p, size, M_WALK) : mymalloc(size, M_WALK);
}

/* Lists and waifs whose last reference is gone but whose contents have
 * not all been freed yet.  Freeing them from here, rather than recursing
 * into each element, keeps a deeply nested list from overflowing the C
 * stack.  A pending list's length counts down as its elements are freed.
 */
static Var *free_pending;
static size_t free_pending_count, free_pending_max;
static int freeing;

static void
free_pending_push(Var v)
{
    if (free_pending_count == free_pending_max) {
	free_pending_max = free_pending_max ? 2 * free_pending_max : 64;
	free_pending = walk_grow(free_pending,
				 free_pending_max * sizeof(Var));
    }
    free_pending[free_pending_count++] = v;
}

static void
free_contents_now(Var v)
{
    if (v.type == TYPE_LIST) {
	Var *list = v.v.list;
	Num i;

	for (i = list[0].v.num; i > 0; i--)
	    free_var(list[i]);
	myfree(list, M_LIST);
    }
#ifdef WAIF_CORE
    else
	free_waif(v.v.waif);
#endif
}

static void
free_contents(Var v)
{
    static int depth;

    if (freeing) {
	free_pending_push(v);
	return;
    }
    /* Recursion is quicker for the shallow lists most values are. */
    if (depth < SMALL_WALK_DEPTH) {
	depth++;
	free_contents_now(v);
	depth--;
	return;
    }
    /* Lists and waifs inside V are only pushed, then freed below. */
    freeing = 1;
    free_contents_now(v);
    while (free_pending_count > 0) {
	Var top = free_pending[free_pending_count - 1];
	size_t mark = free_pending_count;

	if (top.type == TYPE_LIST) {
	    Var *list = top.v.list;

	    while (list[0].v.num > 0 && free_pending_count == mark)
		free_var(list[list[0].v.num--]);
	    if (free_pending_count != mark)
		continue;
	}
	/* Freeing a waif pushes its values, so pop it first. */
	free_pending_count--;
#ifdef WAIF_CORE
	if (top.type == TYPE_WAIF)
	    free_waif(top.v.waif);
	else
#endif
	    myfree(top.v.list, M_LIST);
    }
    freeing = 0;
    if (free_pending_max > 4096) {
	myfree(free_pending, M_WALK);
	free_pending = 0;
	free_pending_max = 0;
    }
}

void
complex_free_var(Var v)
{
    switch (v.type) {
    case TYPE_STR:
	if (v.v.str)
	    free_str(v.v.str);
	break;
    case TYPE_LIST:
	if (delref(v.v.list) == 0)
	    free_contents(v);
	break;
#if FLOATS_ARE_BOXED
    case TYPE_FLOAT:
	if (delref(v.v.fnum) == 0)
	    myfree(v.v.fnum, M_FLOAT);
	break;
#endif
    default:
	break;

#ifdef WAIF_CORE
    case TYPE_WAIF:
	if (delref(v.v.waif) == 0)
	    free_contents(v);
	break;
#endif
#ifdef BOUND_CORE
    case TYPE_BOUND:
	if (delref(v.v.bound) == 0)
	    free_bound(v.v.bound);
	break;
#endif

    }
}

Var
complex_var_ref(Var v)
{
    switch (v.type) {
    case TYPE_STR:
	addref(v.v.str);
	break;
    case TYPE_LIST:
	addref(v.v.list);
	break;
#if FLOATS_ARE_BOXED
    case TYPE_FLOAT:
	addref(v.v.fnum);
	break;
#endif
    default:
	break;

#ifdef WAIF_CORE
    case TYPE_WAIF:
	addref(v.v.waif);
	break;
#endif
#ifdef BOUND_CORE
    case TYPE_BOUND:
	addref(v.v.bound);
	break;
#endif

    }
    return v;
}

Var
complex_var_dup(Var v)
{
    int i;
    Var newlist;

    switch (v.type) {
    case TYPE_STR:
	v.v.str = str_dup(v.v.str);
	break;
    case TYPE_LIST:
	newlist = new_list(v.v.list[0].v.num);
	for (i = 1; i <= v.v.list[0].v.num; i++) {
	    newlist.v.list[i] = var_ref(v.v.list[i]);
	}
	v.v.list = newlist.v.list;
	break;
#if FLOATS_ARE_BOXED
    case TYPE_FLOAT:
	v.v.fnum = box_fl(fl_unbox(v.v.fnum));
	break;
#endif
    default:
	break;

#ifdef WAIF_CORE
    case TYPE_WAIF:
	v.v.waif = dup_waif(v.v.waif);
	break;
#endif
#ifdef BOUND_CORE
    case TYPE_BOUND:
	addref(v.v.bound);
	break;
#endif

    }
    return v;
}

/* could be inlined and use complex_etc like the others, but this should
 * usually be called in a context where we already konw the type.
 */
int
var_refcount(Var v)
{
    switch (v.type) {
    case TYPE_STR:
	return refcount(v.v.str);
    case TYPE_LIST:
	return refcount(v.v.list);
#if FLOATS_ARE_BOXED
    case TYPE_FLOAT:
	return refcount(v.v.fnum);
#endif
#ifdef BOUND_CORE
    case TYPE_BOUND:
	return refcount(v.v.bound);
#endif
    default:
	return 1;
    }
}

int
is_true(Var v)
{
    return ((v.type == TYPE_INT && v.v.num != 0)
	    || (v.type == TYPE_FLOAT && fl_unbox(v.v.fnum) != 0.0)
	    || (v.type == TYPE_STR && v.v.str && *v.v.str != '\0')
	    || (v.type == TYPE_LIST && v.v.list[0].v.num != 0));
}

static int list_equality(Var lhs, Var rhs, int case_matters);

int
equality(Var lhs, Var rhs, int case_matters)
{
    if (lhs.type == TYPE_FLOAT)
	return do_equals(lhs, rhs);
    else if (rhs.type == TYPE_FLOAT)
	return do_equals(rhs, lhs);
    else if (lhs.type != rhs.type)
	return 0;
    else {
	switch (lhs.type) {
	case TYPE_INT:
	    return lhs.v.num == rhs.v.num;
	case TYPE_OBJ:
	    return lhs.v.obj == rhs.v.obj;
	case TYPE_ERR:
	    return lhs.v.err == rhs.v.err;
	case TYPE_STR:
	    if (case_matters)
		return !strcmp(lhs.v.str, rhs.v.str);
	    else
		return !mystrcasecmp(lhs.v.str, rhs.v.str);
	case TYPE_LIST:
	    if (lhs.v.list[0].v.num != rhs.v.list[0].v.num)
		return 0;
	    else if (lhs.v.list == rhs.v.list)
		return 1;
	    else
		return list_equality(lhs, rhs, case_matters);

#ifdef WAIF_CORE
	case TYPE_WAIF:
	    /* compare them or assert same-waif? */
	    return lhs.v.waif == rhs.v.waif;
#endif
#ifdef BOUND_CORE
	case TYPE_BOUND:
	    return bound_equal(lhs.v.bound, rhs.v.bound, case_matters);
#endif

	default:
	    panic("EQUALITY: Unknown value type");
	}
    }
    return 0;
}

/* Returns 1 or 0, or -1 if LHS and RHS are too big to compare this way. */
static int
small_list_equality(Var lhs, Var rhs, int case_matters, int depth,
		    int *budget)
{
    Num i, len = lhs.v.list[0].v.num;

    if (--*budget < 0 || depth > SMALL_WALK_DEPTH)
	return -1;
    for (i = 1; i <= len; i++) {
	Var l = lhs.v.list[i], r = rhs.v.list[i];

	if (l.type == TYPE_LIST && r.type == TYPE_LIST) {
	    int result;

	    if (l.v.list[0].v.num != r.v.list[0].v.num)
		return 0;
	    if (l.v.list == r.v.list)
		continue;
	    result = small_list_equality(l, r, case_matters, depth + 1,
					 budget);
	    if (result != 1)
		return result;
	}
	else if (!equality(l, r, case_matters))
	    return 0;
    }
    return 1;
}

/* Compares two distinct lists of the same length, element by element.
 * Pairs of sublists already found equal are remembered, so lists that
 * share sublists are compared in time proportional to the number of
 * distinct pairs rather than the number of paths.
 */
static int
list_equality(Var lhs, Var rhs, int case_matters)
{
    static Walk_Stack ws;
    Visit_Set equal = {0, 0, 0};
    size_t walked = 0;
    int budget = SMALL_WALK_LISTS;
    int result = small_list_equality(lhs, rhs, case_matters, 0, &budget);

    if (result >= 0)
	return result;
    result = 1;
    walk_push(&ws, lhs, rhs, 0);
    while (ws.depth > 0) {
	Walk_Frame *f = &ws.frames[ws.depth - 1];
	Var l, r;

	if (f->i == f->a.v.list[0].v.num) {
	    if (++walked > WALK_MEMO_AFTER)
		visit_add(&equal, f->a.v.list, f->b.v.list, 0);
	    ws.depth--;
	    continue;
	}
	f->i++;
	l = f->a.v.list[f->i];
	r = f->b.v.list[f->i];
	if (l.type == TYPE_LIST && r.type == TYPE_LIST) {
	    if (l.v.list[0].v.num != r.v.list[0].v.num) {
		result = 0;
		break;
	    }
	    if (l.v.list == r.v.list
		|| (equal.count && visit_find(&equal, l.v.list, r.v.list)))
		continue;
	    walk_push(&ws, l, r, 0);
	}
	else if (!equality(l, r, case_matters)) {
	    result = 0;
	    break;
	}
    }
    walk_release(&ws);
    visit_set_free(&equal);
    return result;
}

void
stream_add_strsub(Stream *str, const char *source, const char *what, const char *with, int case_counts)
{
    int lwhat = strlen(what);

    while (*source) {
	if (!(case_counts ? strncmp(source, what, lwhat)
	      : mystrncasecmp(source, what, lwhat))) {
	    stream_add_string(str, with);
	    source += lwhat;
	} else
	    stream_add_char(str, *source++);
    }
}

int
strindex(const char *source, const char *what, int case_counts)
{
    const char *s, *e;
    int lwhat = strlen(what);
    int ind = 0;

    for (s = source, e = source + strlen(source) - lwhat; s <= e; get_utf(&s), ind++) {
	if (!(case_counts ? strncmp(s, what, lwhat)
	      : mystrncasecmp(s, what, lwhat))) {
	    return ind + 1;
	}
    }
    return 0;
}

int
strrindex(const char *source, const char *what, int case_counts)
{
    const char *s;
    int lwhat = memo_strlen(what);

    for (s = source + memo_strlen(source) - lwhat; s >= source; s--) {
	if (is_utf8_cont_byte(*s))
	    continue;
	if (!(case_counts ? strncmp(s, what, lwhat)
	      : mystrncasecmp(s, what, lwhat))) {
	    return utf_char_index(source, s - source + 1);
	}
    }
    return 0;
}

Var
get_system_property(const char *name)
{
    Var value;
    db_prop_handle h;

    if (!valid(SYSTEM_OBJECT)) {
	value.type = TYPE_ERR;
	value.v.err = E_INVIND;
	return value;
    }
    h = db_find_property(SYSTEM_OBJECT, name, &value);
    if (!h.ptr) {
	value.type = TYPE_ERR;
	value.v.err = E_PROPNF;
    } else if (!h.built_in)	/* make two cases the same */
	value = var_ref(value);
    return value;
}

Objid
get_system_object(const char *name)
{
    Var value;

    value = get_system_property(name);
    if (value.type != TYPE_OBJ) {
	free_var(value);
	return NOTHING;
    } else
	return value.v.obj;
}

/* Bytes for V itself, not counting the elements of a list or the
 * property values of a waif.
 */
static size_t
own_bytes(Var v)
{
    size_t size = BQM_SIZEOF(Var);

    switch (v.type) {
    case TYPE_STR:
	size += memo_strlen(v.v.str) + 1;
	break;
#if BQM_BOXED_FLOATS
    case TYPE_FLOAT:
	size += BQM_SIZEOF(FlNum);
	break;
#endif
    case TYPE_LIST:
	size += BQM_SIZEOF(Var);   /* for the `length' element */
	break;

#ifdef WAIF_CORE
    case TYPE_WAIF:
	size += waif_bytes(v.v.waif);
	break;
#endif
#ifdef BOUND_CORE
    case TYPE_BOUND:
	size += bound_bytes(v.v.bound);
	break;
#endif

    default:
	break;
    }

    return size;
}

/* Returns the size of list V, or 0 if V is too big to size this way. */
static size_t
small_list_bytes(Var v, int depth, int *budget)
{
    Num i, len = v.v.list[0].v.num;
    size_t size = own_bytes(v);

    if (--*budget < 0 || depth > SMALL_WALK_DEPTH)
	return 0;
    for (i = 1; i <= len; i++) {
	Var c = v.v.list[i];

	if (c.type == TYPE_LIST) {
	    size_t n = small_list_bytes(c, depth + 1, budget);

	    if (n == 0)
		return 0;
	    size += n;
	}
	else if (walk_has_children(c))
	    return 0;
	else
	    size += own_bytes(c);
    }
    return size;
}

/* A list or waif that appears several times inside V is counted each
 * time, as if V were written out in full; the total sticks at SIZE_MAX.
 */
size_t
value_bytes(Var v)
{
    static Walk_Stack ws;
    Visit_Set sizes = {0, 0, 0};
    size_t walked = 0, size = 0;
    int budget = SMALL_WALK_LISTS;

    if (!walk_has_children(v))
	return own_bytes(v);
    if (v.type == TYPE_LIST && (size = small_list_bytes(v, 0, &budget)))
	return size;

    walk_push(&ws, v, zero, own_bytes(v));
    while (ws.depth > 0) {
	Walk_Frame *f = &ws.frames[ws.depth - 1];
	Num count;
	Var *children = walk_children(f->a, &count);
	Var c;
	Visit *seen;

	if (f->i == count) {
	    size = f->aux;
	    if (++walked > WALK_MEMO_AFTER)
		visit_add(&sizes, walk_id(f->a), 0, 0)->x = size;
	    if (--ws.depth > 0)
		f[-1].aux = size_add(f[-1].aux, size);
	    continue;
	}
	c = children[f->i++];
	if (!walk_has_children(c))
	    f->aux = size_add(f->aux, own_bytes(c));
	else if (sizes.count && (seen = visit_find(&sizes, walk_id(c), 0)))
	    f->aux = size_add(f->aux, seen->x);
	else
	    walk_push(&ws, c, zero, own_bytes(c));
    }
    walk_release(&ws);
    visit_set_free(&sizes);
    return size;
}

Walk_Frame *
walk_push(Walk_Stack *ws, Var a, Var b, size_t aux)
{
    Walk_Frame *f;

    if (ws->depth == ws->max) {
	ws->max = ws->max ? 2 * ws->max : 64;
	ws->frames = walk_grow(ws->frames, ws->max * sizeof(Walk_Frame));
    }
    f = &ws->frames[ws->depth++];
    f->a = a;
    f->b = b;
    f->i = 0;
    f->aux = aux;
    return f;
}

void
walk_release(Walk_Stack *ws)
{
    ws->depth = 0;
    if (ws->max > 4096) {
	myfree(ws->frames, M_WALK);
	ws->frames = 0;
	ws->max = 0;
    }
}

void
walk_free(Walk_Stack *ws)
{
    if (ws->frames)
	myfree(ws->frames, M_WALK);
    ws->frames = 0;
    ws->depth = ws->max = 0;
}

Var *
walk_children(Var v, Num *count)
{
#ifdef WAIF_CORE
    if (v.type == TYPE_WAIF) {
	int n;
	Var *values = waif_propvals(v.v.waif, &n);

	*count = n;
	return values;
    }
#endif
    *count = v.v.list[0].v.num;
    return v.v.list + 1;
}

static size_t
visit_hash(const void *a, const void *b, size_t size)
{
    uint64_t h = (uintptr_t) a ^ ((uint64_t) (uintptr_t) b * 0xC2B2AE3D27D4EB4FULL);

    h ^= h >> 17;
    h *= 0x9E3779B97F4A7C15ULL;
    return (size_t) (h ^ (h >> 29)) & (size - 1);
}

Visit *
visit_find(const Visit_Set *set, const void *a, const void *b)
{
    size_t i;

    if (!set->size)
	return 0;
    for (i = visit_hash(a, b, set->size); set->slots[i].a;
	 i = (i + 1) & (set->size - 1))
	if (set->slots[i].a == a && set->slots[i].b == b)
	    return &set->slots[i];
    return 0;
}

Visit *
visit_add(Visit_Set *set, const void *a, const void *b, int *added)
{
    Visit *v;
    size_t i;

    if ((v = visit_find(set, a, b))) {
	if (added)
	    *added = 0;
	return v;
    }
    if (2 * (set->count + 1) > set->size) {
	Visit *old = set->slots;
	size_t old_size = set->size, j;

	set->size = old_size ? 2 * old_size : 64;
	set->slots = mymalloc(set->size * sizeof(Visit), M_WALK);
	memset(set->slots, 0, set->size * sizeof(Visit));
	for (j = 0; j < old_size; j++)
	    if (old[j].a) {
		for (i = visit_hash(old[j].a, old[j].b, set->size);
		     set->slots[i].a; i = (i + 1) & (set->size - 1))
		    ;
		set->slots[i] = old[j];
	    }
	if (old)
	    myfree(old, M_WALK);
    }
    for (i = visit_hash(a, b, set->size); set->slots[i].a;
	 i = (i + 1) & (set->size - 1))
	;
    v = &set->slots[i];
    v->a = a;
    v->b = b;
    v->x = v->y = 0;
    set->count++;
    if (added)
	*added = 1;
    return v;
}

void
visit_set_free(Visit_Set *set)
{
    if (set->slots)
	myfree(set->slots, M_WALK);
    set->slots = 0;
    set->size = set->count = 0;
}

void
stream_add_moobinary_from_raw_bytes(Stream *s,
				    const char *buffer, size_t buflen)
{
    size_t i;

    for (i = 0; i < buflen; i++) {
	unsigned char c = buffer[i];

	if (c >= 32 && c < 126)
	    stream_add_char(s, c);
	else
	    stream_printf(s, "~%02x", (int) c);
    }
}

const char *
moobinary_to_raw_bytes(const char *binary, size_t *buflen)
{
    static Stream *s = 0;
    const char *ptr = binary;

    if (!s)
	s = new_stream(100);
    else
	reset_stream(s);

    while (*ptr) {
	unsigned char c = *ptr++;

	if (c != '~')
	    stream_add_char(s, c);
	else {
	    int i;
	    char cc = 0;

	    for (i = 1; i <= 2; i++) {
		/* The | 0x20 changes upper case to lower case for the
		   characters we are interested in here... */
		c = *ptr++ | 0x20;
		if ('0' <= c && c <= '9')
		    cc = (cc << 4) + (c - '0');
		else if ('a' <= c && c <= 'f')
		    cc = (cc << 4) + (c - 'a' + 10);
		else
		    return 0;
	    }

	    stream_add_char(s, cc);
	}
    }

    *buflen = stream_length(s);
    return reset_stream(s);
}

int
stream_add_recoded_chars(Stream *s,
			 const char *inbuf, size_t inbytesleft,
			 const char *fromcode, const char *tocode)
{
    iconv_t cd;
    char *outbuf;
    size_t outbytesleft, room;
    volatile size_t need = inbytesleft * 2;
    volatile int ret;

    cd = iconv_open(tocode, fromcode);
    if (cd == (iconv_t) -1)
	return 0;

    TRY
	do {
	    stream_beginfill(s, need, &outbuf, &outbytesleft);
	    room = outbytesleft;
	    ret = (size_t) -1 !=
		iconv(cd, (void *)&inbuf, &inbytesleft,
		      /*  Evidently, Solaris wants inbuf
			  to be const. Oh, well... */
		      &outbuf, &outbytesleft);

	    stream_endfill(s, outbytesleft);

	    /* The next character can need more room than is left (one
	     * ASCII byte becomes four bytes of UTF-32, for instance).
	     * If nothing fit, ask for more than we had; otherwise
	     * stream_beginfill() can hand back the same too-small space
	     * and we would retry forever.
	     */
	    need = outbytesleft == room ? 2 * room + 16 : inbytesleft * 2;
	}
	while (!ret && errno == E2BIG);
	/* E2BIG = remaining output buffer too small => try again
	 * die on all other errors, including
	 *   EILSEQ: invalid input sequence
	 *   EINVAL: incomplete input sequence
	 */
    FINALLY
	iconv_close(cd);
    ENDTRY;

    return ret;
}


/*
 * $Log$
 * Revision 2.8  1996/04/08  00:43:09  pavel
 * Changed definition of `value_bytes()' to add in `sizeof(Var)'.
 * Release 1.8.0p3.
 *
 * Revision 2.7  1996/03/11  23:34:41  pavel
 * Changed %X to %x in a stream_printf call, since I don't want to support
 * both upper- and lower-case.  Release 1.8.0p1.
 *
 * Revision 2.6  1996/03/10  01:14:16  pavel
 * Change the format of binary strings to use hex instead of octal.
 * Release 1.8.0.
 *
 * Revision 2.5  1996/02/08  06:41:04  pavel
 * Added support for floating-point.  Moved compare_ints() to numbers.c.
 * Renamed err/logf() to errlog/oklog() and TYPE_NUM to TYPE_INT.  Updated
 * copyright notice for 1996.  Release 1.8.0beta1.
 *
 * Revision 2.4  1996/01/16  07:24:48  pavel
 * Removed special format for `~' in binary strings.  Release 1.8.0alpha6.
 *
 * Revision 2.3  1996/01/11  07:40:01  pavel
 * Added raw_bytes_to_binary() and binary_to_raw_bytes(), in support of binary
 * I/O facilities.  Release 1.8.0alpha5.
 *
 * Revision 2.2  1995/12/28  00:38:54  pavel
 * Neatened up implementation of case-folding string comparison functions.
 * Release 1.8.0alpha3.
 *
 * Revision 2.1  1995/12/11  08:09:31  pavel
 * Account for newly-clarified reference-counting behavior for built-in
 * properties.  Add `value_bytes()' from elsewhere.  Release 1.8.0alpha2.
 *
 * Revision 2.0  1995/11/30  04:43:24  pavel
 * New baseline version, corresponding to release 1.8.0alpha1.
 *
 * Revision 1.13  1992/10/23  23:03:47  pavel
 * Added copyright notice.
 *
 * Revision 1.12  1992/10/23  22:23:18  pavel
 * Eliminated all uses of the useless macro NULL.
 *
 * Revision 1.11  1992/10/17  20:57:08  pavel
 * Global rename of strdup->str_dup, strref->str_ref, vardup->var_dup, and
 * varref->var_ref.
 * Removed useless #ifdef of STRCASE.
 *
 * Revision 1.10  1992/09/14  17:39:53  pjames
 * Moved db_modification code to db modules.
 *
 * Revision 1.9  1992/09/08  21:55:47  pjames
 * Updated #includes.
 *
 * Revision 1.8  1992/09/03  16:23:49  pjames
 * Make cmap[] visible.  Added TYPE_CLEAR handling.
 *
 * Revision 1.7  1992/08/28  16:23:41  pjames
 * Changed vardup to varref.
 * Changed myfree(*, M_STRING) to free_str(*).
 * Added `varref()'.
 * Changed `free_var()' to check `delref()' before freeing.
 * Removed `copy_pi()'.
 *
 * Revision 1.6  1992/08/18  00:54:40  pavel
 * Fixed typo.
 *
 * Revision 1.5  1992/08/18  00:41:14  pavel
 * Fixed boundary-condition bugs in index() and rindex(), when the search string
 * is empty.
 *
 * Revision 1.4  1992/08/11  17:26:56  pjames
 * Removed read/write Parse_Info procedures.
 *
 * Revision 1.3  1992/08/10  16:40:20  pjames
 * Added is_true (from execute.c) and *_activ_as_pi routines.  Added a
 * check for null parse_info in write_pi, because parse_infos are no
 * longer stored in each activation.
 *
 * Revision 1.2  1992/07/21  00:07:48  pavel
 * Added rcsid_<filename-root> declaration to hold the RCS ident. string.
 *
 * Revision 1.1  1992/07/20  23:23:12  pavel
 * Initial RCS-controlled version.
 */
