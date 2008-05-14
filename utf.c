/*
 * utf.c
 *
 * Routines related to UTF-8 handing
 */

#include <string.h>
#include <inttypes.h>
#include "utf.h"

/*
 * get_utf():
 *
 * Gets a single UTF-8 character from a string.
 */
#define GET_CONT \
    c = *p;		      \
    c -= 0x80;		      \
    if ( c > 0x3f )	      \
	goto bad_cont;	      \
    p++;		      \
    v = (v << 6) + c;

int get_utf(const char **pp)
{
    const char *p = *pp;
    unsigned char c;
    int v;

    c = *p++;

    if (c <= 0x7f) {
	v = c;
    } else if (c <= 0xbf) {
	v = INVALID_RUNE;
    } else if (c <= 0xdf) {
	v = c & 0x1f;
	GET_CONT;
	if (v <= 0x7f)
	    v = INVALID_RUNE;
    } else if (c <= 0xef) {
	v = c & 0x0f;
	GET_CONT;
	GET_CONT;
	if (v <= 0x7ff || (v >= 0xd800 && v <= 0xdfff))
	    v = INVALID_RUNE;
    } else if (c <= 0xf7) {
	v = c & 0x07;
	GET_CONT;
	GET_CONT;
	GET_CONT;
	if (v <= 0xffff || v > 0x10ffff)
	    v = INVALID_RUNE;
    } else {
	v = INVALID_RUNE;
    }

  done:
    *pp = p;
    return v;

  bad_cont:
    v = INVALID_RUNE;
    goto done;
}

#undef GET_CONT

/*
 * get_utf_call():
 *
 * Get a single UTF-8 character from a generic client.
 */
#define GET_CONT \
    c = c_getch(c_data);      \
    cc = c;		      \
    c -= 0x80;		      \
    if ( c > 0x3f )	      \
	goto bad_cont;	      \
    v = (v << 6) + c;

int get_utf_call(int (*c_getch) (void *), void *c_data, int *state)
{
    int c;
    int cc = *state;
    int v;

    if (cc != -1) {
	c = cc;
	cc = -1;
    } else {
	c = c_getch(c_data);
    }

    if (c <= 0x7f) {
	v = c;
    } else if (c <= 0xbf) {
	v = INVALID_RUNE;
    } else if (c <= 0xdf) {
	v = c & 0x1f;
	GET_CONT;
	if (v <= 0x7f)
	    v = INVALID_RUNE;
    } else if (c <= 0xef) {
	v = c & 0x0f;
	GET_CONT;
	GET_CONT;
	if (v <= 0x7ff || (v >= 0xd800 && v <= 0xdfff))
	    v = INVALID_RUNE;
    } else if (c <= 0xf7) {
	v = c & 0x07;
	GET_CONT;
	GET_CONT;
	GET_CONT;
	if (v <= 0xffff || v > 0x10ffff)
	    v = INVALID_RUNE;
    } else {
	v = INVALID_RUNE;
    }

    *state = -1;

  done:
    return v;

  bad_cont:
    *state = cc;
    v = INVALID_RUNE;
    goto done;
}

int put_utf(char **pp, int v)
{
    char *p = *pp;

    if (v < 0 || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
	return -1;		/* Invalid UCS */

    if (v <= 0x7f) {
	*p++ = v;
    } else if (v <= 0x7ff) {
	*p++ = 0xc0 | (v >> 6);
	*p++ = 0x80 | (v & 0x3f);
    } else if (v <= 0xffff) {
	*p++ = 0xe0 | (v >> 12);
	*p++ = 0x80 | ((v >> 6) & 0x3f);
	*p++ = 0x80 | (v & 0x3f);
    } else {
	*p++ = 0xf0 | (v >> 18);
	*p++ = 0x80 | ((v >> 12) & 0x3f);
	*p++ = 0x80 | ((v >> 6) & 0x3f);
	*p++ = 0x80 | (v & 0x3f);
    }

    *pp = p;

    return 0;
}
