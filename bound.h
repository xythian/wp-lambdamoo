#ifndef Bound_H
#define Bound_H 1

#include "config.h"
#include "options.h"
#include "functions.h"
#include "structures.h"
#include "streams.h"

#ifdef BOUND_CORE

#define BOUND_OP_PUBLIC  (1U << 0)
#define BOUND_OP_WRITE   (1U << 1)

typedef package (*bound_verb_handler)(BoundValue *value, Var arglist,
                                      Objid progr);
typedef enum error (*bound_prop_handler)(BoundValue *bound, Var *value,
                                         Objid progr, int write);

typedef struct BoundEncoder BoundEncoder;
typedef struct BoundDecoder BoundDecoder;

typedef struct {
    size_t length;
    enum error (*read_at)(void *token, size_t offset,
                          void *destination, size_t length);
    void (*release)(void *token);
    void *token;
} BoundByteSource;

typedef struct {
    const char *name;
    bound_verb_handler handler;
    unsigned flags;
    int minargs;
    int maxargs;
    var_type prototype[4];
} BoundVerbDef;

typedef struct {
    const char *name;
    bound_prop_handler handler;
    unsigned flags;
} BoundPropDef;

typedef struct BoundTypeDef {
    const char *name;
    unsigned schema_version;
    const BoundVerbDef *class_verbs;
    const BoundVerbDef *verbs;
    const BoundPropDef *properties;

    package (*construct)(Var arglist, Objid progr);
    void (*destroy)(void *payload);
    enum error (*encode)(void *payload, BoundEncoder *out);
    enum error (*decode)(unsigned schema, BoundDecoder *in, void **payload);
    size_t (*bytes)(void *payload);
    int (*equal)(void *lhs, void *rhs, int case_matters);
    void (*hash)(void *payload, Stream *out);
    enum error (*output)(void *payload, const char **bytes, size_t *length,
                         void **token, void (**release)(void *));
    enum error (*byte_source)(void *payload, BoundByteSource *source);
    enum error (*input)(void *payload, const char *data, size_t length,
                        int binary, size_t *written);
} BoundTypeDef;

extern int register_bound_type(const BoundTypeDef *);
extern Var new_bound_value(const char *type_name, Objid owner, void *payload);
extern Var bound_type_handle(const char *type_name);
extern const char *bound_type_name(BoundValue *);
extern Objid bound_owner(BoundValue *);
extern void *bound_payload(BoundValue *);
extern int bound_is_available(BoundValue *);

extern enum error bound_encoder_write(BoundEncoder *, const void *, size_t);
extern unsigned bound_encoder_add_ref(BoundEncoder *, Var);
extern const void *bound_decoder_bytes(BoundDecoder *, size_t *);
extern unsigned bound_decoder_ref_count(BoundDecoder *);
extern enum error bound_decoder_ref(BoundDecoder *, unsigned, Var *);

extern void free_bound(BoundValue *);
extern int bound_equal(BoundValue *, BoundValue *, int);
extern int bound_bytes(BoundValue *);
extern void bound_hash(BoundValue *, Stream *);
extern void bound_tostr(BoundValue *, Stream *);
extern void bound_toliteral(BoundValue *, Stream *);

extern enum error bound_get_prop(BoundValue *, const char *, Var *, Objid);
extern enum error bound_put_prop(BoundValue *, const char *, Var, Objid);
extern package bound_call_verb(BoundValue *, const char *, Var, Objid);

extern enum error bound_output_view(BoundValue *, const char **, size_t *,
                                    void **, void (**)(void *));
extern enum error bound_byte_source(BoundValue *, BoundByteSource *);
extern enum error bound_input_sink(BoundValue *, const char *, size_t, int, size_t *);

extern void dbio_write_bound(Var);
extern int dbio_read_bound(Var *);

#endif /* BOUND_CORE */

#endif /* Bound_H */
