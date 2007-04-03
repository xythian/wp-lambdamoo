/*
 * utf-ctype.c
 *
 * Invoke libucd to test for specific character classes.
 */

#include "config.h"
#include "ucd.h"
#include "my-ctype.h"

int my_tolower(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd)
	return x;

    rv = ucd->simple_lowercase;
    unicode_character_put(ucd);

    return rv;
}

int my_toupper(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd)
	return x;

    rv = ucd->simple_uppercase;
    unicode_character_put(ucd);

    return rv;
}

int my_isdigit(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd)
	return 0;

    rv = ucd->numeric_type == UC_NT_Decimal;
    unicode_character_put(ucd);

    return rv;
}

int my_digitval(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd || ucd->numeric_type != UC_NT_Decimal)
	return 0;

    /* For digits, den == exp == 1 */
    rv = ucd->numeric_value_num;
    unicode_character_put(ucd);

    return rv;
}

int my_isspace(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd)
	return x;

    rv = !!(ucd->fl & UC_FL_WHITE_SPACE);
    unicode_character_put(ucd);

    return rv;
}

/*
 * The XID categories are the best thing in Unicode to what
 * characters should allow to begin and continue identifiers,
 * respectively.
 */
int my_is_xid_start(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd)
	return 0;

    rv = !!(ucd->fl & UC_FL_XID_START);
    unicode_character_put(ucd);

    return rv;
}

int my_is_xid_cont(int x)
{
    const struct unicode_character_data *ucd;
    int rv;

    ucd = unicode_character_data(x);
    if (!ucd)
	return 0;

    rv = !!(ucd->fl & UC_FL_XID_CONTINUE);
    unicode_character_put(ucd);

    return rv;
}
