/*
 * Text builtins that matter mostly for Unicode strings
 *
 * Each function also works without Unicode, where every character is
 * a single ASCII character.  With Unicode, the character data comes
 * from the same library as utf-ctype.c (--with-uclib).
 */

#include "bf_register.h"

#include "config.h"
#include "options.h"

#if !UNICODE_STRINGS
#elif UNICODE_DATA == UD_GNU
#  include <unigbrk.h>
#  include <uniwidth.h>
#elif UNICODE_DATA == UD_ICU
#  include <unicode/ubrk.h>
#  include <unicode/uchar.h>
#  include <unicode/utext.h>
#else
#  error "the unitext extension needs --with-uclib=gnu or --with-uclib=icu"
#endif

#include "my-string.h"

#include "exceptions.h"
#include "functions.h"
#include "list.h"
#include "storage.h"
#include "streams.h"
#include "structures.h"
#include "utf.h"
#include "utils.h"

/*------------------------*
 |   grapheme clusters    |
 *------------------------*/

/*  Set p[i] nonzero exactly where a grapheme cluster (a user-perceived
 *  character, per UAX #29) starts at byte i of s[0..n), n > 0.
 */
static void
grapheme_breaks(const char *s, size_t n, char *p)
{
#if !UNICODE_STRINGS
    (void)s;
    memset(p, 1, n);

#elif UNICODE_DATA == UD_GNU
    u8_grapheme_breaks((const uint8_t *)s, n, p);

#elif UNICODE_DATA == UD_ICU
    static UBreakIterator *bi = NULL;
    UErrorCode err = U_ZERO_ERROR;
    UText *ut = utext_openUTF8(NULL, s, n, &err);

    if (!bi)
	bi = ubrk_open(UBRK_CHARACTER, NULL, NULL, 0, &err);
    ubrk_setUText(bi, ut, &err);
    if (U_FAILURE(err))
	panic("ICU error in grapheme_breaks()");

    memset(p, 0, n);
    for (int32_t b = ubrk_first(bi); b != UBRK_DONE; b = ubrk_next(bi))
	if ((size_t)b < n)
	    p[b] = 1;
    utext_close(ut);

#endif
}

/*  Call each(start, end, data) for every grapheme cluster of str,
 *  in order.
 */
static void
for_each_grapheme(const char *str,
		  void (*each)(const char *, const char *, void *),
		  void *data)
{
    size_t n = memo_strlen(str);
    if (n == 0)
	return;

    char *p = mymalloc(n, M_STRING);
    grapheme_breaks(str, n, p);

    size_t start = 0;
    for (size_t i = 1; i <= n; i++)
	if (i == n || p[i]) {
	    each(str + start, str + i, data);
	    start = i;
	}
    myfree(p, M_STRING);
}

static void
append_grapheme(const char *start, const char *end, void *data)
{
    Var *list = data;
    Stream *s = new_stream(end - start);
    Var v;

    stream_add_bytes(s, start, end - start);
    v.type = TYPE_STR;
    v.v.str = str_dup_then_free_stream(s);
    *list = listappend(*list, v);
}

static package
bf_graphemes(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    Var r = new_list(0);

    for_each_grapheme(arglist.v.list[1].v.str, append_grapheme, &r);
    free_var(arglist);
    return make_var_pack(r);
}

/*-------------------*
 |   display width   |
 *-------------------*/

/*  Number of columns a terminal uses to display x:  0 for combining
 *  marks and other zero-width characters, 2 for East Asian Wide and
 *  Fullwidth characters (which include most emoji), 1 otherwise.  Tab
 *  counts as 1, since its true width depends on where it lands.
 */
static int
char_width(uint32_t x)
{
#if !UNICODE_STRINGS
    (void)x;
    return 1;

#else
    if (x == 0x09)
	return 1;

#  if   UNICODE_DATA == UD_GNU
    return uc_width(x, "UTF-8");

#  elif UNICODE_DATA == UD_ICU
    switch (u_charType(x)) {
    case U_NON_SPACING_MARK:
    case U_ENCLOSING_MARK:
    case U_FORMAT_CHAR:
	return 0;
    default:
	break;
    }
    switch (u_getIntPropertyValue(x, UCHAR_HANGUL_SYLLABLE_TYPE)) {
    case U_HST_VOWEL_JAMO:
    case U_HST_TRAILING_JAMO:
	return 0;
    default:
	break;
    }
    switch (u_getIntPropertyValue(x, UCHAR_EAST_ASIAN_WIDTH)) {
    case U_EA_WIDE:
    case U_EA_FULLWIDTH:
	return 2;
    default:
	return 1;
    }

#  endif
#endif /* UNICODE_STRINGS */
}

#define is_regional_indicator(x)  ((x) >= 0x1F1E6 && (x) <= 0x1F1FF)

/*  A grapheme cluster takes the width of its first character that has
 *  one, so combining marks, skin-tone modifiers and the rest of an emoji
 *  ZWJ sequence add nothing.  An emoji presentation selector (U+FE0F)
 *  or a pair of regional indicators (a flag) makes it two columns.
 */
static void
add_grapheme_width(const char *start, const char *end, void *data)
{
    Num *width = data;
    uint32_t first = get_utf(&start);
    int w = char_width(first);
    int pair = 0;

    while (start < end) {
	uint32_t ucs = get_utf(&start);
	if (ucs == 0xFE0F) {
	    *width += 2;
	    return;
	}
	if (is_regional_indicator(first) && is_regional_indicator(ucs))
	    pair = 1;
	if (!w)
	    w = char_width(ucs);
    }
    *width += pair ? 2 : w;
}

static package
bf_string_width(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    Num width = 0;

    for_each_grapheme(arglist.v.list[1].v.str, add_grapheme_width, &width);
    free_var(arglist);
    return make_int_pack(width);
}

void
register_unitext(void)
{
    register_function("graphemes", 1, 1, bf_graphemes, TYPE_STR);
    register_function("string_width", 1, 1, bf_string_width, TYPE_STR);
}
