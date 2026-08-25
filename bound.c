#include "bound.h"

#include "config.h"
#include "options.h"

#ifdef BOUND_CORE

#include "bf_register.h"
#include "db.h"
#include "db_io.h"
#include "db_private.h"
#include "exceptions.h"
#include "list.h"
#include "log.h"
#include "ref_count.h"
#include "storage.h"
#include "tasks.h"
#include "streams.h"
#include "utils.h"

typedef struct BoundRegistryEntry BoundRegistryEntry;

struct BoundRegistryEntry {
    const char *name;
    const BoundTypeDef *def;
    BoundRegistryEntry *next;
};

#define BV_CLASS  1U
#define BV_OPAQUE 2U
#define BOUND_MAX_BLOB (64U * 1024U * 1024U)
#define BOUND_MAX_REFS (1024U * 1024U)

typedef struct BoundEnvelope {
    char *blob;
    size_t blob_length;
    Var *refs;
    unsigned ref_count;
} BoundEnvelope;

struct BoundEncoder {
    Stream *blob;
    Var *refs;
    unsigned ref_count;
    unsigned ref_capacity;
};

struct BoundDecoder {
    const BoundEnvelope *envelope;
};

struct BoundValue {
    BoundRegistryEntry *entry;
    Objid owner;
    void *payload;
    BoundEnvelope envelope;
    unsigned schema;
    unsigned flags;
    unsigned save_generation;
    unsigned save_index;
    BoundValue *next;
    BoundValue **prev;
};

static BoundRegistryEntry *registry;
static BoundValue *all_values;
static unsigned bound_count;
static unsigned save_generation;
static unsigned next_save_index;
static BoundValue **loaded_values;
static unsigned loaded_count;
static unsigned loaded_capacity;

static BoundRegistryEntry *
find_entry(const char *name)
{
    BoundRegistryEntry *e;

    for (e = registry; e; e = e->next)
        if (!mystrcasecmp(e->name, name))
            return e;
    return NULL;
}

static BoundRegistryEntry *
find_or_create_entry(const char *name)
{
    BoundRegistryEntry *e = find_entry(name);

    if (!e) {
        e = mymalloc(sizeof(*e), M_BOUND_XTRA);
        e->name = str_dup(name);
        e->def = NULL;
        e->next = registry;
        registry = e;
    }
    return e;
}

static void
link_value(BoundValue *v)
{
    v->prev = &all_values;
    v->next = all_values;
    if (all_values)
        all_values->prev = &v->next;
    all_values = v;
    bound_count++;
}

static Var
alloc_bound(BoundRegistryEntry *entry, Objid owner)
{
    BoundValue *v = mymalloc(sizeof(*v), M_BOUND);
    Var result;

    v->entry = entry;
    v->owner = owner;
    v->payload = NULL;
    v->envelope.blob = NULL;
    v->envelope.blob_length = 0;
    v->envelope.refs = NULL;
    v->envelope.ref_count = 0;
    v->schema = entry->def ? entry->def->schema_version : 0;
    v->flags = 0;
    v->save_generation = 0;
    v->save_index = 0;
    link_value(v);

    result.type = TYPE_BOUND;
    result.v.bound = v;
    return result;
}

static void
envelope_free(BoundEnvelope *e)
{
    unsigned i;

    if (e->blob)
        myfree(e->blob, M_STREAM);
    for (i = 0; i < e->ref_count; i++)
        free_var(e->refs[i]);
    if (e->refs)
        myfree(e->refs, M_BOUND_XTRA);
    e->blob = NULL;
    e->blob_length = 0;
    e->refs = NULL;
    e->ref_count = 0;
}

enum error
bound_encoder_write(BoundEncoder *e, const void *bytes, size_t length)
{
    if (!e || (!bytes && length))
        return E_INVARG;
    if (length)
        stream_add_bytes(e->blob, bytes, length);
    return E_NONE;
}

unsigned
bound_encoder_add_ref(BoundEncoder *e, Var value)
{
    if (!e || e->ref_count >= BOUND_MAX_REFS)
        return UINT_MAX;
    if (e->ref_count == e->ref_capacity) {
        unsigned capacity = e->ref_capacity ? e->ref_capacity * 2 : 4;
        if (capacity < e->ref_capacity)
            return UINT_MAX;
        e->refs = e->refs
            ? myrealloc(e->refs, capacity * sizeof(Var), M_BOUND_XTRA)
            : mymalloc(capacity * sizeof(Var), M_BOUND_XTRA);
        e->ref_capacity = capacity;
    }
    e->refs[e->ref_count] = var_ref(value);
    return e->ref_count++;
}

const void *
bound_decoder_bytes(BoundDecoder *d, size_t *length)
{
    if (!d)
        return NULL;
    if (length)
        *length = d->envelope->blob_length;
    return d->envelope->blob;
}

unsigned
bound_decoder_ref_count(BoundDecoder *d)
{
    return d ? d->envelope->ref_count : 0;
}

enum error
bound_decoder_ref(BoundDecoder *d, unsigned index, Var *out)
{
    if (!d || !out || index >= d->envelope->ref_count)
        return E_RANGE;
    *out = var_ref(d->envelope->refs[index]);
    return E_NONE;
}

static enum error
decode_envelope(const BoundTypeDef *def, unsigned schema,
                BoundEnvelope *envelope, void **payload)
{
    BoundDecoder decoder;
    decoder.envelope = envelope;
    return def->decode(schema, &decoder, payload);
}

int
register_bound_type(const BoundTypeDef *def)
{
    BoundRegistryEntry *e;
    BoundValue *v;

    if (!def || !def->name || !*def->name || !def->decode || !def->encode)
        return 0;
    e = find_or_create_entry(def->name);
    if (e->def)
        return 0;
    e->def = def;

    for (v = all_values; v; v = v->next) {
        if (v->entry != e || !(v->flags & BV_OPAQUE) || (v->flags & BV_CLASS))
            continue;
        void *payload = NULL;
        if (decode_envelope(def, v->schema, &v->envelope, &payload) == E_NONE) {
            envelope_free(&v->envelope);
            v->payload = payload;
            v->flags &= ~BV_OPAQUE;
        }
    }
    return 1;
}

Var
new_bound_value(const char *type_name, Objid owner, void *payload)
{
    BoundRegistryEntry *e = find_entry(type_name);
    Var result;

    if (!e || !e->def)
        return zero;
    result = alloc_bound(e, owner);
    result.v.bound->payload = payload;
    return result;
}

Var
bound_type_handle(const char *type_name)
{
    BoundRegistryEntry *e = find_entry(type_name);
    Var result;

    if (!e)
        return zero;
    result = alloc_bound(e, SYSTEM_OBJECT);
    result.v.bound->flags = BV_CLASS;
    return result;
}

const char *
bound_type_name(BoundValue *v)
{
    return v->entry->name;
}

Objid
bound_owner(BoundValue *v)
{
    return v->owner;
}

void *
bound_payload(BoundValue *v)
{
    return v->payload;
}

int
bound_is_available(BoundValue *v)
{
    return v->entry->def != NULL && !(v->flags & BV_OPAQUE);
}

void
free_bound(BoundValue *v)
{
    if (!(v->flags & BV_CLASS)) {
        if (v->flags & BV_OPAQUE)
            envelope_free(&v->envelope);
        else if (v->entry->def && v->entry->def->destroy)
            v->entry->def->destroy(v->payload);
    }
    *v->prev = v->next;
    if (v->next)
        v->next->prev = v->prev;
    bound_count--;
    myfree(v, M_BOUND);
}

int
bound_equal(BoundValue *a, BoundValue *b, int case_matters)
{
    if (a == b)
        return 1;
    if (a->entry != b->entry || a->flags != b->flags)
        return 0;
    if ((a->flags & (BV_CLASS | BV_OPAQUE)) || !a->entry->def
        || !a->entry->def->equal)
        return 0;
    return a->entry->def->equal(a->payload, b->payload, case_matters);
}

int
bound_bytes(BoundValue *v)
{
    size_t total = sizeof(*v) + strlen(v->entry->name) + 1;

    if (v->flags & BV_OPAQUE)
        total += v->envelope.blob_length
            + v->envelope.ref_count * sizeof(Var);
    else if (!(v->flags & BV_CLASS) && v->entry->def && v->entry->def->bytes)
        total += v->entry->def->bytes(v->payload);
    return total > INT_MAX ? INT_MAX : (int)total;
}

void
bound_hash(BoundValue *v, Stream *s)
{
    stream_add_string(s, v->entry->name);
    stream_add_char(s, ':');
    if (!(v->flags & (BV_CLASS | BV_OPAQUE))
        && v->entry->def && v->entry->def->hash)
        v->entry->def->hash(v->payload, s);
}

void
bound_tostr(BoundValue *v, Stream *s)
{
    stream_add_string(s, "{bound ");
    stream_add_string(s, v->entry->name);
    if (v->flags & BV_CLASS)
        stream_add_string(s, " class");
    else if (!bound_is_available(v))
        stream_add_string(s, " unavailable");
    stream_add_char(s, '}');
}

void
bound_toliteral(BoundValue *v, Stream *s)
{
    stream_add_string(s, "[[bound ");
    stream_add_string(s, v->entry->name);
    stream_add_string(s, "]]");
}

static int
controls(BoundValue *v, Objid progr)
{
    return is_wizard(progr) || progr == v->owner;
}

static int
check_args(const BoundVerbDef *verb, Var args)
{
    int nargs = args.v.list[0].v.num;
    int i, count = verb->maxargs == -1 ? verb->minargs : nargs;

    if (nargs < verb->minargs || (verb->maxargs != -1 && nargs > verb->maxargs))
        return E_ARGS;
    if (count > 4)
        count = 4;
    for (i = 0; i < count; i++) {
        var_type expected = verb->prototype[i];
        var_type actual = args.v.list[i + 1].type;
        if (expected != TYPE_ANY && expected != actual
            && !(expected == TYPE_NUMERIC
                 && (actual == TYPE_INT || actual == TYPE_FLOAT)))
            return E_TYPE;
    }
    return E_NONE;
}

static const BoundVerbDef *
find_verb(const BoundVerbDef *verbs, const char *name)
{
    if (!verbs)
        return NULL;
    while (verbs->name) {
        if (!mystrcasecmp(verbs->name, name))
            return verbs;
        verbs++;
    }
    return NULL;
}

static const BoundPropDef *
find_prop(const BoundPropDef *props, const char *name)
{
    if (!props)
        return NULL;
    while (props->name) {
        if (!mystrcasecmp(props->name, name))
            return props;
        props++;
    }
    return NULL;
}

static Var
names_for_verbs(const BoundVerbDef *verbs)
{
    int n = 0, i;
    Var result;

    if (verbs)
        while (verbs[n].name)
            n++;
    result = new_list(n);
    for (i = 0; i < n; i++) {
        result.v.list[i + 1].type = TYPE_STR;
        result.v.list[i + 1].v.str = str_dup(verbs[i].name);
    }
    return result;
}

static Var
names_for_props(const BoundPropDef *props)
{
    int n = 0, i;
    Var result;

    if (props)
        while (props[n].name)
            n++;
    result = new_list(n);
    for (i = 0; i < n; i++) {
        result.v.list[i + 1].type = TYPE_STR;
        result.v.list[i + 1].v.str = str_dup(props[i].name);
    }
    return result;
}

enum error
bound_get_prop(BoundValue *v, const char *name, Var *out, Objid progr)
{
    const BoundPropDef *prop;

    if (!mystrcasecmp(name, "owner")) {
        out->type = TYPE_OBJ;
        out->v.obj = v->owner;
        return E_NONE;
    }
    if (!mystrcasecmp(name, "class")) {
        *out = bound_type_handle(v->entry->name);
        return out->type == TYPE_BOUND ? E_NONE : E_INVIND;
    }
    if ((v->flags & BV_CLASS) && !mystrcasecmp(name, "name")) {
        out->type = TYPE_STR;
        out->v.str = str_dup(v->entry->name);
        return E_NONE;
    }
    if ((v->flags & BV_CLASS) && !mystrcasecmp(name, "available")) {
        out->type = TYPE_INT;
        out->v.num = v->entry->def != NULL;
        return E_NONE;
    }
    if (!bound_is_available(v))
        return E_INVIND;
    prop = find_prop(v->entry->def->properties, name);
    if (!prop)
        return E_PROPNF;
    if (!(prop->flags & BOUND_OP_PUBLIC) && !controls(v, progr))
        return E_PERM;
    return prop->handler(v, out, progr, 0);
}

enum error
bound_put_prop(BoundValue *v, const char *name, Var value, Objid progr)
{
    const BoundPropDef *prop;

    if (v->flags & BV_CLASS)
        return E_PERM;
    if (!bound_is_available(v))
        return E_INVIND;
    prop = find_prop(v->entry->def->properties, name);
    if (!prop)
        return E_PROPNF;
    if (!(prop->flags & BOUND_OP_WRITE))
        return E_PERM;
    if (!(prop->flags & BOUND_OP_PUBLIC) && !controls(v, progr))
        return E_PERM;
    return prop->handler(v, &value, progr, 1);
}

package
bound_call_verb(BoundValue *v, const char *name, Var args, Objid progr)
{
    const BoundTypeDef *def = v->entry->def;
    const BoundVerbDef *verb;
    enum error e;

    if (!def || (v->flags & BV_OPAQUE)) {
        free_var(args);
        return make_error_pack(E_INVIND);
    }
    if ((v->flags & BV_CLASS) && !mystrcasecmp(name, "new")) {
        if (!def->construct) {
            free_var(args);
            return make_error_pack(E_VERBNF);
        }
        return def->construct(args, progr);
    }
    if ((v->flags & BV_CLASS) && !mystrcasecmp(name, "verbs")) {
        free_var(args);
        return make_var_pack(names_for_verbs(def->verbs));
    }
    if ((v->flags & BV_CLASS) && !mystrcasecmp(name, "properties")) {
        free_var(args);
        return make_var_pack(names_for_props(def->properties));
    }
    verb = find_verb(v->flags & BV_CLASS ? def->class_verbs : def->verbs, name);
    if (!verb) {
        free_var(args);
        return make_error_pack(E_VERBNF);
    }
    if (!(verb->flags & BOUND_OP_PUBLIC) && !controls(v, progr)) {
        free_var(args);
        return make_error_pack(E_PERM);
    }
    e = check_args(verb, args);
    if (e != E_NONE) {
        free_var(args);
        return make_error_pack(e);
    }
    return verb->handler(v, args, progr);
}

enum error
bound_output_view(BoundValue *v, const char **bytes, size_t *length,
                  void **token, void (**release)(void *))
{
    const BoundTypeDef *def = v->entry->def;

    if ((v->flags & (BV_CLASS | BV_OPAQUE)) || !def || !def->output)
        return E_TYPE;
    return def->output(v->payload, bytes, length, token, release);
}

enum error
bound_byte_source(BoundValue *v, BoundByteSource *source)
{
    const BoundTypeDef *def = v->entry->def;
    enum error e;

    source->length = 0;
    source->read_at = NULL;
    source->release = NULL;
    source->token = NULL;
    if ((v->flags & (BV_CLASS | BV_OPAQUE)) || !def || !def->byte_source)
        return E_TYPE;
    e = def->byte_source(v->payload, source);
    if (e != E_NONE)
        return e;
    if (!source->read_at || !source->release) {
        if (source->release)
            source->release(source->token);
        source->length = 0;
        source->read_at = NULL;
        source->release = NULL;
        source->token = NULL;
        return E_INVARG;
    }
    return E_NONE;
}

enum error
bound_input_sink(BoundValue *v, const char *data, size_t length, int binary,
                 size_t *written)
{
    const BoundTypeDef *def = v->entry->def;
    if ((v->flags & (BV_CLASS | BV_OPAQUE)) || !def || !def->input)
        return E_TYPE;
    return def->input(v->payload, data, length, binary, written);
}

static package
bf_bound_type(Var args, Byte next UNUSED_, void *data UNUSED_, Objid progr UNUSED_)
{
    Var result = bound_type_handle(args.v.list[1].v.str);
    free_var(args);
    if (result.type != TYPE_BOUND)
        return make_error_pack(E_INVARG);
    return make_var_pack(result);
}

static package
bf_bound_types(Var args, Byte next UNUSED_, void *data UNUSED_, Objid progr UNUSED_)
{
    BoundRegistryEntry *e;
    int n = 0, i = 1;
    Var result;

    free_var(args);
    for (e = registry; e; e = e->next)
        if (e->def)
            n++;
    result = new_list(n);
    for (e = registry; e; e = e->next)
        if (e->def)
            result.v.list[i++] = bound_type_handle(e->name);
    return make_var_pack(result);
}

static void
before_save(void)
{
    save_generation++;
    if (!save_generation)
        save_generation++;
    next_save_index = 0;
}

static void
after_save(int success UNUSED_)
{
}

static void
before_load(void)
{
    loaded_count = 0;
    loaded_capacity = 32;
    loaded_values = mymalloc(loaded_capacity * sizeof(*loaded_values),
                             M_BOUND_XTRA);
}

static void
after_load(int success UNUSED_)
{
    myfree(loaded_values, M_BOUND_XTRA);
    loaded_values = NULL;
    loaded_count = loaded_capacity = 0;
}

static Var
encode_blob(const char *data, size_t length)
{
    Stream *out = new_stream(length * 3 + 1);
    Var encoded;

    stream_add_moobinary_from_raw_bytes(out, data, length);
    encoded.type = TYPE_STR;
    encoded.v.str = str_dup(stream_contents(out));
    free_stream(out);
    return encoded;
}

static int
decode_blob(const char *encoded, size_t expected, char **out)
{
    const char *raw;
    size_t length;

    raw = moobinary_to_raw_bytes(encoded, &length);
    if (!raw || length != expected)
        return 0;
    *out = mymalloc(length + 1, M_STREAM);
    memcpy(*out, raw, length);
    (*out)[length] = 0;
    return 1;
}

void
dbio_write_bound(Var value)
{
    BoundValue *v = value.v.bound;
    BoundEnvelope envelope = {NULL, 0, NULL, 0};
    BoundEncoder encoder;
    Var encoded_blob;
    unsigned i;
    enum error e;

    if (v->save_generation == save_generation) {
        dbio_printf("r %u\n", v->save_index);
        return;
    }
    v->save_generation = save_generation;
    v->save_index = next_save_index++;
    dbio_printf("c %u\n", v->save_index);
    dbio_write_string(v->entry->name);
    dbio_printf("%u\n", v->schema);
    dbio_write_objid(v->owner);
    dbio_printf("%u\n", v->flags & BV_CLASS);

    if (v->flags & BV_OPAQUE)
        envelope = v->envelope;
    else if (!(v->flags & BV_CLASS)) {
        if (!v->entry->def) {
            errlog("BOUND: no provider for %s value\n", v->entry->name);
            RAISE(dbpriv_dbio_failed, 0);
            return;
        }
        encoder.blob = new_stream(64);
        encoder.refs = NULL;
        encoder.ref_count = encoder.ref_capacity = 0;
        e = v->entry->def->encode(v->payload, &encoder);
        if (e == E_NONE && stream_length(encoder.blob) > BOUND_MAX_BLOB)
            e = E_QUOTA;
        if (e != E_NONE) {
            for (i = 0; i < encoder.ref_count; i++)
                free_var(encoder.refs[i]);
            if (encoder.refs)
                myfree(encoder.refs, M_BOUND_XTRA);
            free_stream(encoder.blob);
            errlog("BOUND: cannot serialize %s value\n", v->entry->name);
            RAISE(dbpriv_dbio_failed, 0);
            return;
        }
        envelope.blob = stream_detach(encoder.blob, &envelope.blob_length);
        envelope.refs = encoder.refs;
        envelope.ref_count = encoder.ref_count;
    }

    dbio_printf("%zu\n", envelope.blob_length);
    encoded_blob = encode_blob(envelope.blob ? envelope.blob : "",
                               envelope.blob_length);
    dbio_write_string(encoded_blob.v.str);
    free_var(encoded_blob);
    dbio_printf("%u\n", envelope.ref_count);
    for (i = 0; i < envelope.ref_count; i++)
        dbio_write_var(envelope.refs[i]);
    dbio_printf(".\n");

    if (!(v->flags & (BV_CLASS | BV_OPAQUE)))
        envelope_free(&envelope);
}

int
dbio_read_bound(Var *out)
{
    char kind;
    unsigned index, i;

    if (!dbio_scxnf("%c %u", &kind, &index))
        return 0;
    if (kind == 'r') {
        if (index >= loaded_count || !loaded_values[index])
            return 0;
        out->type = TYPE_BOUND;
        out->v.bound = loaded_values[index];
        addref(out->v.bound);
        return 1;
    }
    if (kind != 'c' || index != loaded_count)
        return 0;

    const char *name, *blob_string;
    intmax_t schema, class_flag, blob_length, ref_count;
    Objid owner;
    BoundEnvelope envelope = {NULL, 0, NULL, 0};
    if (!dbio_read_string_intern(&name)
        || !dbio_read_intmax(&schema)
        || schema < 0 || schema > UINT_MAX
        || !dbio_read_objid(&owner)
        || !dbio_read_intmax(&class_flag)
        || (class_flag != 0 && class_flag != BV_CLASS))
        return 0;

    BoundRegistryEntry *entry = find_or_create_entry(name);
    Var result = alloc_bound(entry, owner);
    BoundValue *v = result.v.bound;
    v->schema = schema;
    if (class_flag)
        v->flags |= BV_CLASS;

    if (loaded_count == loaded_capacity) {
        loaded_capacity *= 2;
        loaded_values = myrealloc(loaded_values,
                                   loaded_capacity * sizeof(*loaded_values),
                                   M_BOUND_XTRA);
    }
    loaded_values[loaded_count++] = v;

    if (!dbio_read_intmax(&blob_length) || blob_length < 0
        || (uintmax_t) blob_length > SIZE_MAX
        || (uintmax_t) blob_length > BOUND_MAX_BLOB
        || !dbio_read_string_intern(&blob_string))
        return 0;
    envelope.blob_length = (size_t) blob_length;
    if (!decode_blob(blob_string, envelope.blob_length, &envelope.blob)) {
        free_str(blob_string);
        return 0;
    }
    free_str(blob_string);
    if (!dbio_read_intmax(&ref_count) || ref_count < 0
        || ref_count > BOUND_MAX_REFS) {
        envelope_free(&envelope);
        return 0;
    }
    envelope.ref_count = (unsigned) ref_count;
    if (envelope.ref_count)
        envelope.refs = mymalloc(envelope.ref_count * sizeof(Var), M_BOUND_XTRA);
    for (i = 0; i < envelope.ref_count; i++) {
        envelope.refs[i].type = TYPE_NONE;
        if (!dbio_read_var(&envelope.refs[i])) {
            envelope.ref_count = i;
            envelope_free(&envelope);
            return 0;
        }
    }
    if (!dbio_scxnf(".")) {
        envelope_free(&envelope);
        return 0;
    }

    if (!(v->flags & BV_CLASS) && entry->def) {
        void *payload = NULL;
        enum error e = decode_envelope(entry->def, v->schema, &envelope,
                                       &payload);
        if (e == E_NONE) {
            v->payload = payload;
            envelope_free(&envelope);
        } else {
            v->flags |= BV_OPAQUE;
            v->envelope = envelope;
        }
    } else if (!(v->flags & BV_CLASS)) {
        v->flags |= BV_OPAQUE;
        v->envelope = envelope;
    } else
        envelope_free(&envelope);
    *out = result;
    return 1;
}

void
register_bound(void)
{
    register_function("bound_type", 1, 1, bf_bound_type, TYPE_STR);
    register_function("bound_types", 0, 0, bf_bound_types);
    register_db_load_hooks(900, before_load, after_load,
                           "restore bound values");
    register_db_save_hooks(900, before_save, after_save,
                           "save bound values");
}

#endif /* BOUND_CORE */


#ifndef BOUND_CORE
void register_bound(void);
void register_bound(void) { }
#endif
