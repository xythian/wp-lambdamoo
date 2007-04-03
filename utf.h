/*
 * utf.h
 *
 * Prototypes for UTF-8 handling
 */

#ifndef UTF_H
#define UTF_H

#define INVALID_RUNE	0xfffd

int get_utf(const char **);
int get_utf_call(int (*)(void *), void *, int *);
int put_utf(char **, int);

#endif /* UTF_H */
