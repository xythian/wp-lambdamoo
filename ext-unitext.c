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
#  include <uniwidth.h>
#elif UNICODE_DATA == UD_ICU
#  include <unicode/uchar.h>
#else
#  error "the unitext extension needs --with-uclib=gnu or --with-uclib=icu"
#endif

#include "functions.h"
#include "structures.h"
#include "utf.h"
#include "utils.h"

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

static package
bf_string_width(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    const char *s = arglist.v.list[1].v.str;
    Num width = 0;
    uint32_t ucs;

    while ((ucs = get_utf(&s)))
	width += char_width(ucs);
    free_var(arglist);
    return make_int_pack(width);
}

void
register_unitext(void)
{
    register_function("string_width", 1, 1, bf_string_width, TYPE_STR);
}
