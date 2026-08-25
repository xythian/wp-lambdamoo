#ifndef Ext_Bound_Stream_H
#define Ext_Bound_Stream_H 1

#include "bound.h"

#if defined(BOUND_CORE) && defined(BOUND_STREAM)
extern Var make_bound_bytes_copy(Objid owner, const void *data, size_t length);
extern Var make_bound_bytes_take(Objid owner, char *data, size_t length,
                                 void (*release)(void *), void *token);
#endif

#endif /* Ext_Bound_Stream_H */
