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
#  include <unicase.h>
#  include <unictype.h>
#  include <unigbrk.h>
#  include <unilbrk.h>
#  include <uninorm.h>
#  include <uniwidth.h>
#elif UNICODE_DATA == UD_ICU
#  include <unicode/ubrk.h>
#  include <unicode/uchar.h>
#  include <unicode/unorm2.h>
#  include <unicode/uscript.h>
#  include <unicode/ustring.h>
#  include <unicode/utext.h>
#else
#  error "the unitext extension needs --with-uclib=gnu or --with-uclib=icu"
#endif

#include "my-ctype.h"
#include "my-stdlib.h"
#include "my-string.h"

#include "exceptions.h"
#include "functions.h"
#include "list.h"
#include "storage.h"
#include "streams.h"
#include "structures.h"
#include "utf-ctype.h"
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

/*  Return a buffer of memo_strlen(str) flags, nonzero where a grapheme
 *  cluster starts, or NULL for the empty string.  Free with myfree(,
 *  M_STRING).
 */
static char *
grapheme_starts(const char *str)
{
    size_t n = memo_strlen(str);
    if (n == 0)
	return NULL;

    char *p = mymalloc(n, M_STRING);
    grapheme_breaks(str, n, p);
    return p;
}

typedef void (*grapheme_fn)(const char *start, const char *end, void *data);

/*  Call each(start, end, data) for every grapheme cluster of str, in
 *  order, given p from grapheme_starts(str).
 */
static void
walk_graphemes(const char *str, const char *p, grapheme_fn each, void *data)
{
    size_t n = memo_strlen(str);
    size_t start = 0;

    for (size_t i = 1; i <= n; i++)
	if (i == n || p[i]) {
	    each(str + start, str + i, data);
	    start = i;
	}
}

static void
for_each_grapheme(const char *str, grapheme_fn each, void *data)
{
    char *p = grapheme_starts(str);
    if (!p)
	return;

    walk_graphemes(str, p, each, data);
    myfree(p, M_STRING);
}

struct grapheme_list {
    Var list;
    int next;
};

static void
store_grapheme(const char *start, const char *end, void *data)
{
    struct grapheme_list *gl = data;
    Stream *s = new_stream(end - start);
    Var *v = &gl->list.v.list[++gl->next];

    stream_add_bytes(s, start, end - start);
    v->type = TYPE_STR;
    v->v.str = str_dup_then_free_stream(s);
}

static package
bf_graphemes(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    const char *str = arglist.v.list[1].v.str;
    size_t n = memo_strlen(str);
    char *p = grapheme_starts(str);
    int count = 0;

    for (size_t i = 0; i < n; i++)
	count += !!p[i];

    struct grapheme_list gl = { new_list(count), 0 };
    if (p) {
	walk_graphemes(str, p, store_grapheme, &gl);
	myfree(p, M_STRING);
    }
    free_var(arglist);
    return make_var_pack(gl.list);
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
static int
grapheme_width(const char *start, const char *end)
{
    uint32_t first = get_utf(&start);
    int w = char_width(first);
    int pair = 0;

    while (start < end) {
	uint32_t ucs = get_utf(&start);
	if (ucs == 0xFE0F)
	    return 2;
	if (is_regional_indicator(first) && is_regional_indicator(ucs))
	    pair = 1;
	if (!w)
	    w = char_width(ucs);
    }
    return pair ? 2 : w;
}

static void
add_grapheme_width(const char *start, const char *end, void *data)
{
    Num *width = data;
    *width += grapheme_width(start, end);
}

static package
bf_string_width(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    Num width = 0;

    for_each_grapheme(arglist.v.list[1].v.str, add_grapheme_width, &width);
    free_var(arglist);
    return make_int_pack(width);
}

/*-------------------*
 |   line wrapping   |
 *-------------------*/

/*  Set p[i] nonzero exactly where s[0..n), n > 0, may be broken
 *  before byte i to start a new line (per UAX #14).
 */
static void
line_breaks(const char *s, size_t n, char *p)
{
#if !UNICODE_STRINGS
    p[0] = 0;
    for (size_t i = 1; i < n; i++)
	p[i] = s[i] != ' ' && (s[i - 1] == ' ' || s[i - 1] == '-');

#elif UNICODE_DATA == UD_GNU
    u8_possible_linebreaks((const uint8_t *)s, n, "UTF-8", p);
    for (size_t i = 0; i < n; i++)
	p[i] = p[i] == UC_BREAK_POSSIBLE;

#elif UNICODE_DATA == UD_ICU
    static UBreakIterator *bi = NULL;
    UErrorCode err = U_ZERO_ERROR;
    UText *ut = utext_openUTF8(NULL, s, n, &err);

    if (!bi)
	bi = ubrk_open(UBRK_LINE, NULL, NULL, 0, &err);
    ubrk_setUText(bi, ut, &err);
    if (U_FAILURE(err))
	panic("ICU error in line_breaks()");

    memset(p, 0, n);
    for (int32_t b = ubrk_first(bi); b != UBRK_DONE; b = ubrk_next(bi))
	if (b > 0 && (size_t)b < n)
	    p[b] = 1;
    utext_close(ut);

#endif
}

static int
is_space_grapheme(const char *start, const char *end)
{
    uint32_t ucs = get_utf(&start);
    return start == end && my_isspace(ucs);
}

struct wrap_state {
    const char *str;
    const char *breaks;	/* from line_breaks() */
    Num width;		/* maximum columns per line */
    Var lines;
    size_t line_start;	/* offset where the current line starts */
    Num line_width;	/* columns of str[line_start..] so far */
    size_t cut;		/* last break opportunity in the line, or 0 */
    Num cut_width;	/* columns of str[line_start..cut] */
};

/*  Add str[start..end) to the result, without trailing spaces.
 */
static void
emit_line(struct wrap_state *ws, size_t start, size_t end)
{
    const char *s = ws->str;

    while (end > start) {
	const char *e = s + end;
	const char *b = e - 1;
	while (b > s + start && is_utf8_cont_byte(*b))
	    b--;
	if (!is_space_grapheme(b, e))
	    break;
	end = b - s;
    }
    if (end == start)
	return;

    Stream *line = new_stream(end - start + 1);
    Var v;

    stream_add_bytes(line, s + start, end - start);
    v.type = TYPE_STR;
    v.v.str = str_dup_then_free_stream(line);
    ws->lines = listappend(ws->lines, v);
}

/*  Fit the grapheme cluster str[start..end) onto the current line,
 *  first ending the line at its last break opportunity (or right
 *  here, if it has none) when the cluster would make it too wide.
 *  Spaces never make a line too wide; they are dropped from the end
 *  of a line instead.
 */
static void
wrap_grapheme(const char *start, const char *end, void *data)
{
    struct wrap_state *ws = data;
    size_t at = start - ws->str;
    int w = grapheme_width(start, end);

    if (ws->breaks[at] && at > ws->line_start) {
	ws->cut = at;
	ws->cut_width = ws->line_width;
    }
    if (is_space_grapheme(start, end)) {
	ws->line_width += w;
	return;
    }
    if (ws->line_width + w > ws->width && ws->cut > ws->line_start) {
	emit_line(ws, ws->line_start, ws->cut);
	ws->line_start = ws->cut;
	ws->line_width -= ws->cut_width;
    }
    if (ws->line_width + w > ws->width && at > ws->line_start) {
	emit_line(ws, ws->line_start, at);
	ws->line_start = at;
	ws->line_width = 0;
    }
    ws->line_width += w;
}

static package
bf_wrap(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    const char *str = arglist.v.list[1].v.str;
    size_t n = memo_strlen(str);
    struct wrap_state ws;

    if (arglist.v.list[2].v.num < 1) {
	free_var(arglist);
	return make_error_pack(E_INVARG);
    }

    ws.str = str;
    ws.width = arglist.v.list[2].v.num;
    ws.lines = new_list(0);
    ws.line_start = ws.cut = 0;
    ws.line_width = ws.cut_width = 0;

    char *p = grapheme_starts(str);
    if (p) {
	char *b = mymalloc(n, M_STRING);
	line_breaks(str, n, b);
	ws.breaks = b;
	walk_graphemes(str, p, wrap_grapheme, &ws);
	emit_line(&ws, ws.line_start, n);
	myfree(b, M_STRING);
	myfree(p, M_STRING);
    }
    free_var(arglist);
    return make_var_pack(ws.lines);
}

/*--------------------------------------*
 |   normalization and case folding     |
 *--------------------------------------*/

enum transform {
    XF_NFC, XF_NFD, XF_NFKC, XF_NFKD, XF_CASEFOLD
};

#if UNICODE_STRINGS && UNICODE_DATA == UD_ICU
/*  Convert s[0..n) to UTF-16 in a buffer from mymalloc(, M_STRING),
 *  setting *len to its length.
 */
static UChar *
icu_from_utf8(const char *s, size_t n, int32_t *len)
{
    UErrorCode err = U_ZERO_ERROR;
    u_strFromUTF8(NULL, 0, len, s, n, &err);
    UChar *u = mymalloc((*len + 1) * sizeof(UChar), M_STRING);

    err = U_ZERO_ERROR;
    u_strFromUTF8(u, *len + 1, NULL, s, n, &err);
    if (U_FAILURE(err))
	panic("ICU error in icu_from_utf8()");
    return u;
}

static int32_t
icu_apply(enum transform xf, const UChar *src, int32_t n,
	  UChar *dst, int32_t cap, UErrorCode *err)
{
    const UNormalizer2 *norm = NULL;

    switch (xf) {
    case XF_CASEFOLD:
	return u_strFoldCase(dst, cap, src, n, U_FOLD_CASE_DEFAULT, err);
    case XF_NFC:  norm = unorm2_getNFCInstance(err);  break;
    case XF_NFD:  norm = unorm2_getNFDInstance(err);  break;
    case XF_NFKC: norm = unorm2_getNFKCInstance(err); break;
    case XF_NFKD: norm = unorm2_getNFKDInstance(err); break;
    }
    return unorm2_normalize(norm, src, n, dst, cap, err);
}
#endif

/*  Append the result of applying xf to s[0..n) to out.
 */
static void
stream_add_transformed(Stream *out, enum transform xf, const char *s, size_t n)
{
#if !UNICODE_STRINGS
    if (xf != XF_CASEFOLD) {
	stream_add_bytes(out, s, n);
	return;
    }
    for (size_t i = 0; i < n; i++)
	stream_add_char(out, tolower((unsigned char)s[i]));

#elif UNICODE_DATA == UD_GNU
    size_t len;
    uint8_t *r;

    if (xf == XF_CASEFOLD)
	r = u8_casefold((const uint8_t *)s, n, NULL, NULL, NULL, &len);
    else
	r = u8_normalize(xf == XF_NFC  ? UNINORM_NFC  :
			 xf == XF_NFD  ? UNINORM_NFD  :
			 xf == XF_NFKC ? UNINORM_NFKC : UNINORM_NFKD,
			 (const uint8_t *)s, n, NULL, &len);
    if (!r)
	panic("libunistring out of memory in stream_add_transformed()");
    stream_add_bytes(out, (const char *)r, len);
    free(r);

#elif UNICODE_DATA == UD_ICU
    int32_t ulen, rlen;
    UChar *u = icu_from_utf8(s, n, &ulen);
    UErrorCode err = U_ZERO_ERROR;

    rlen = icu_apply(xf, u, ulen, NULL, 0, &err);
    UChar *r = mymalloc((rlen + 1) * sizeof(UChar), M_STRING);
    err = U_ZERO_ERROR;
    icu_apply(xf, u, ulen, r, rlen + 1, &err);
    if (U_FAILURE(err))
	panic("ICU error in stream_add_transformed()");
    myfree(u, M_STRING);

    int32_t blen;
    char *b;
    size_t cap;
    err = U_ZERO_ERROR;
    u_strToUTF8(NULL, 0, &blen, r, rlen, &err);
    stream_beginfill(out, blen, &b, &cap);
    err = U_ZERO_ERROR;
    u_strToUTF8(b, cap, NULL, r, rlen, &err);
    if (U_FAILURE(err))
	panic("ICU error in stream_add_transformed()");
    stream_endfill(out, cap - blen);
    myfree(r, M_STRING);

#endif
}

/*  Return a package of str transformed by xf, or E_QUOTA when the
 *  result is longer than the server allows a string to be.
 */
static package
transformed_pack(enum transform xf, const char *str)
{
    Stream *s = new_stream(memo_strlen(str) + 1);

    stream_add_transformed(s, xf, str, memo_strlen(str));
    if (stream_length(s) >= stream_alloc_maximum) {
	free_stream(s);
	return make_space_pack();
    }
    return make_string_pack(str_dup_then_free_stream(s));
}

static package
bf_normalize(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    static const struct {
	const char *name;
	enum transform xf;
    } forms[] = {
	{ "NFC", XF_NFC }, { "NFD", XF_NFD },
	{ "NFKC", XF_NFKC }, { "NFKD", XF_NFKD },
    };
    enum transform xf = XF_NFC;

    if (arglist.v.list[0].v.num == 2) {
	const char *form = arglist.v.list[2].v.str;
	size_t i;

	for (i = 0; i < sizeof(forms) / sizeof(*forms); i++)
	    if (!mystrcasecmp(form, forms[i].name))
		break;
	if (i == sizeof(forms) / sizeof(*forms)) {
	    free_var(arglist);
	    return make_error_pack(E_INVARG);
	}
	xf = forms[i].xf;
    }

    package p = transformed_pack(xf, arglist.v.list[1].v.str);
    free_var(arglist);
    return p;
}

static package
bf_casefold(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    package p = transformed_pack(XF_CASEFOLD, arglist.v.list[1].v.str);
    free_var(arglist);
    return p;
}

/*------------------------------*
 |   character category/script  |
 *------------------------------*/

/*  The character of a one-character string, or 0 for any other string.
 */
static uint32_t
only_char(const char *s)
{
    uint32_t ucs = get_utf(&s);
    return *s ? 0 : ucs;
}

#if !UNICODE_STRINGS
/*  General categories of the ASCII punctuation and symbols, from
 *  '!' (0x21) through '~' (0x7e); letters and digits are blank here.
 */
static const char *const ascii_category[] = {
    "Po", "Po", "Po", "Sc", "Po", "Po", "Po", "Ps",	/* !"#$%&'( */
    "Pe", "Po", "Sm", "Po", "Pd", "Po", "Po",		/* )*+,-./  */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,			/* 0-9      */
    "Po", "Po", "Sm", "Sm", "Sm", "Po", "Po",		/* :;<=>?@  */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* A-Z      */
    "Ps", "Po", "Pe", "Sk", "Pc", "Sk",			/* [\]^_`   */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,		/* a-z      */
    "Ps", "Sm", "Pe", "Sm",				/* {|}~     */
};
#endif

/*  The two-letter Unicode General Category of x, e.g. "Lu".
 */
static const char *
char_category(uint32_t x)
{
#if !UNICODE_STRINGS
    if (isupper(x))
	return "Lu";
    if (islower(x))
	return "Ll";
    if (isdigit(x))
	return "Nd";
    if (x == ' ')
	return "Zs";
    if (x > ' ' && x < 0x7f)
	return ascii_category[x - '!'];
    return "Cc";

#elif UNICODE_DATA == UD_GNU
    return uc_general_category_name(uc_general_category(x));

#elif UNICODE_DATA == UD_ICU
    return u_getPropertyValueName(UCHAR_GENERAL_CATEGORY, u_charType(x),
				  U_SHORT_PROPERTY_NAME);

#endif
}

/*  The Unicode Script of x, e.g. "Latin", "Common" or "Unknown".
 */
static const char *
char_script(uint32_t x)
{
#if !UNICODE_STRINGS
    return isalpha(x) ? "Latin" : "Common";

#elif UNICODE_DATA == UD_GNU
    const uc_script_t *script = uc_script(x);
    return script ? script->name : "Unknown";

#elif UNICODE_DATA == UD_ICU
    UErrorCode err = U_ZERO_ERROR;
    UScriptCode code = uscript_getScript(x, &err);
    return U_FAILURE(err) ? "Unknown" : uscript_getName(code);

#endif
}

static package
bf_char_category(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    uint32_t ucs = only_char(arglist.v.list[1].v.str);
    free_var(arglist);
    if (!ucs)
	return make_error_pack(E_INVARG);
    return make_string_pack(str_dup(char_category(ucs)));
}

static package
bf_char_script(Var arglist, Byte next UNUSED_, void *vdata UNUSED_, Objid progr UNUSED_)
{
    uint32_t ucs = only_char(arglist.v.list[1].v.str);
    free_var(arglist);
    if (!ucs)
	return make_error_pack(E_INVARG);
    return make_string_pack(str_dup(char_script(ucs)));
}

void
register_unitext(void)
{
    register_function("casefold", 1, 1, bf_casefold, TYPE_STR);
    register_function("char_category", 1, 1, bf_char_category, TYPE_STR);
    register_function("char_script", 1, 1, bf_char_script, TYPE_STR);
    register_function("graphemes", 1, 1, bf_graphemes, TYPE_STR);
    register_function("normalize", 1, 2, bf_normalize, TYPE_STR, TYPE_STR);
    register_function("string_width", 1, 1, bf_string_width, TYPE_STR);
    register_function("wrap", 2, 2, bf_wrap, TYPE_STR, TYPE_INT);
}
