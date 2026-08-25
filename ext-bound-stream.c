#include "ext-bound-stream.h"

#include "config.h"
#include "options.h"

void register_bound_stream_types(void);

#if defined(BOUND_CORE) && defined(BOUND_STREAM)

#include "exceptions.h"
#include "log.h"
#include "storage.h"
#include "tasks.h"
#include "utils.h"


typedef struct ByteStore {
    unsigned refs;
    char *data;
    size_t length;
    void (*release)(void *);
    void *release_token;
} ByteStore;

typedef struct BoundStream {
    Stream *open;
    ByteStore *sealed;
    const char *encoding;
} BoundStream;

static ByteStore *
byte_store_new(char *data, size_t length)
{
    ByteStore *s = mymalloc(sizeof(*s), M_BOUND_XTRA);
    s->refs = 1;
    s->data = data;
    s->length = length;
    s->release = NULL;
    s->release_token = NULL;
    return s;
}

static void
byte_store_retain(void *p)
{
    ((ByteStore *) p)->refs++;
}

static void
byte_store_release(void *p)
{
    ByteStore *s = p;
    if (--s->refs == 0) {
        if (s->release)
            s->release(s->release_token);
        else
            myfree(s->data, M_STREAM);
        myfree(s, M_BOUND_XTRA);
    }
}

Var
make_bound_bytes_take(Objid owner, char *data, size_t length,
                      void (*release)(void *), void *token)
{
    ByteStore *store;
    Var result;

    if (length && !data) {
        result.type = TYPE_ERR;
        result.v.err = E_INVARG;
        return result;
    }
    store = byte_store_new(data, length);
    store->release = release;
    store->release_token = token;
    result = new_bound_value("bytes", owner, store);
    if (result.type != TYPE_BOUND) {
        byte_store_release(store);
        result.type = TYPE_ERR;
        result.v.err = E_INVARG;
    }
    return result;
}

Var
make_bound_bytes_copy(Objid owner, const void *data, size_t length)
{
    char *copy;
    Var result;

    if (length && !data) {
        result.type = TYPE_ERR;
        result.v.err = E_INVARG;
        return result;
    }
    if (length == (size_t) -1) {
        result.type = TYPE_ERR;
        result.v.err = E_QUOTA;
        return result;
    }
    copy = mymalloc(length + 1, M_STREAM);
    if (length)
        memcpy(copy, data, length);
    copy[length] = 0;
    return make_bound_bytes_take(owner, copy, length, NULL, NULL);
}

static const char *
moo_character_encoding(void)
{
#if UNICODE_STRINGS
    return "UTF-8";
#else
    return "ISO-8859-1";
#endif
}

static enum error
append_binary_value(Stream *out, Var v)
{
    int i;
    size_t length;
    const char *raw;

    switch (v.type) {
    case TYPE_INT:
        if (v.v.num < 0 || v.v.num > 255)
            return E_INVARG;
        stream_add_char(out, (char) v.v.num);
        return E_NONE;
    case TYPE_STR:
        raw = moobinary_to_raw_bytes(v.v.str, &length);
        if (!raw)
            return E_INVARG;
        stream_add_bytes(out, raw, length);
        return E_NONE;
    case TYPE_LIST:
        for (i = 1; i <= v.v.list[0].v.num; i++) {
            enum error e = append_binary_value(out, v.v.list[i]);
            if (e != E_NONE)
                return e;
        }
        return E_NONE;
    default:
        return E_TYPE;
    }
}

static enum error
append_text_value(Stream *out, Var v)
{
    int i;

    switch (v.type) {
    case TYPE_INT:
#if UNICODE_STRINGS
        if (v.v.num < 0 || v.v.num > 0x10ffff
            || stream_add_utf(out, v.v.num))
            return E_INVARG;
#else
        if (v.v.num < 0 || v.v.num > 255)
            return E_INVARG;
        stream_add_char(out, (char) v.v.num);
#endif
        return E_NONE;
    case TYPE_STR:
        stream_add_string(out, v.v.str);
        return E_NONE;
    case TYPE_LIST:
        for (i = 1; i <= v.v.list[0].v.num; i++) {
            enum error e = append_text_value(out, v.v.list[i]);
            if (e != E_NONE)
                return e;
        }
        return E_NONE;
    default:
        return E_TYPE;
    }
}

static Var
make_binary_string(const char *data, size_t length)
{
    Stream *out = new_stream(length * 3 + 1);
    Var r;
    stream_add_moobinary_from_raw_bytes(out, data, length);
    r.type = TYPE_STR;
    r.v.str = str_dup(stream_contents(out));
    free_stream(out);
    return r;
}

static enum error
bytes_encode(void *payload, BoundEncoder *out)
{
    ByteStore *s = payload;
    return bound_encoder_write(out, s->data, s->length);
}

static enum error
bytes_decode(unsigned schema, BoundDecoder *in, void **payload)
{
    const char *raw;
    char *copy;
    size_t length = 0;

    if (schema != 1 || bound_decoder_ref_count(in) != 0)
        return E_INVARG;
    raw = bound_decoder_bytes(in, &length);
    copy = mymalloc(length + 1, M_STREAM);
    if (length)
        memcpy(copy, raw, length);
    copy[length] = '\0';
    *payload = byte_store_new(copy, length);
    return E_NONE;
}

static size_t
bytes_size(void *payload)
{
    ByteStore *s = payload;
    return sizeof(*s) + s->length;
}

static int
bytes_equal(void *left, void *right, int case_matters)
{
    ByteStore *a = left, *b = right;
    (void) case_matters;
    return a->length == b->length && !memcmp(a->data, b->data, a->length);
}

static void
bytes_hash(void *payload, Stream *out)
{
    ByteStore *s = payload;
    stream_add_bytes(out, s->data, s->length);
}

static enum error
bytes_output(void *payload, const char **data, size_t *length,
             void **token, void (**release)(void *))
{
    ByteStore *s = payload;
    byte_store_retain(s);
    *data = s->data;
    *length = s->length;
    *token = s;
    *release = byte_store_release;
    return E_NONE;
}

static enum error
byte_store_read_at(void *token, size_t offset, void *destination, size_t length)
{
    ByteStore *s = token;

    if (offset > s->length || length > s->length - offset)
        return E_RANGE;
    if (length && !destination)
        return E_INVARG;
    if (length)
        memcpy(destination, s->data + offset, length);
    return E_NONE;
}

static enum error
byte_store_source(ByteStore *s, BoundByteSource *source)
{
    byte_store_retain(s);
    source->length = s->length;
    source->read_at = byte_store_read_at;
    source->release = byte_store_release;
    source->token = s;
    return E_NONE;
}

static enum error
bytes_source(void *payload, BoundByteSource *source)
{
    return byte_store_source(payload, source);
}

static package
bytes_construct(Var args, Objid progr)
{
    Stream *out = new_stream(32);
    enum error e = E_NONE;
    size_t length;
    char *data;
    ByteStore *store;
    Var result;
    int i;

    for (i = 1; i <= args.v.list[0].v.num && e == E_NONE; i++)
        e = append_binary_value(out, args.v.list[i]);
    free_var(args);
    if (e != E_NONE) {
        free_stream(out);
        return make_error_pack(e);
    }
    data = stream_detach(out, &length);
    store = byte_store_new(data, length);
    result = new_bound_value("bytes", progr, store);
    return make_var_pack(result);
}

static package
bytes_binary_string(BoundValue *value, Var args, Objid progr)
{
    ByteStore *s = bound_payload(value);
    (void) progr;
    free_var(args);
    return make_var_pack(make_binary_string(s->data, s->length));
}

static package
bytes_string(BoundValue *value, Var args, Objid progr)
{
    ByteStore *s = bound_payload(value);
    const char *encoding = args.v.list[0].v.num ? args.v.list[1].v.str : "UTF-8";
    Stream *out = new_stream(s->length + 1);
    Var result;
    (void) progr;

    if (!stream_add_recoded_chars(out, s->data, s->length, encoding,
                                  moo_character_encoding())
        || memchr(stream_contents(out), '\0', stream_length(out))) {
        free_stream(out);
        free_var(args);
        return make_error_pack(E_INVARG);
    }
    result.type = TYPE_STR;
    result.v.str = str_dup(stream_contents(out));
    free_stream(out);
    free_var(args);
    return make_var_pack(result);
}

static enum error
bytes_length_prop(BoundValue *value, Var *out, Objid progr, int write)
{
    ByteStore *s = bound_payload(value);
    (void) progr;
    (void) write;
    out->type = TYPE_INT;
    out->v.num = s->length;
    return E_NONE;
}

static void
bound_stream_destroy(void *payload)
{
    BoundStream *s = payload;
    if (s->open)
        free_stream(s->open);
    if (s->sealed)
        byte_store_release(s->sealed);
    free_str(s->encoding);
    myfree(s, M_BOUND_XTRA);
}

static const char *
bound_stream_data(BoundStream *s)
{
    return s->open ? stream_contents(s->open) : s->sealed->data;
}

static size_t
bound_stream_length(BoundStream *s)
{
    return s->open ? stream_length(s->open) : s->sealed->length;
}

static enum error
bound_stream_source(void *payload, BoundByteSource *source)
{
    BoundStream *s = payload;

    if (!s->sealed)
        return E_INVARG;
    return byte_store_source(s->sealed, source);
}

static package
bound_stream_construct(Var args, Objid progr)
{
    int nargs = args.v.list[0].v.num;
    BoundStream *s;
    Var result;

    if (nargs > 1 || (nargs == 1 && args.v.list[1].type != TYPE_STR)) {
        free_var(args);
        return make_error_pack(nargs > 1 ? E_ARGS : E_TYPE);
    }
    s = mymalloc(sizeof(*s), M_BOUND_XTRA);
    s->open = new_stream(32);
    s->sealed = NULL;
    s->encoding = str_dup(nargs ? args.v.list[1].v.str : "UTF-8");
    free_var(args);
    result = new_bound_value("stream", progr, s);
    return make_var_pack(result);
}

static package
bound_stream_append_binary(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    const char *raw;
    size_t length;
    (void) progr;

    if (!s->open) {
        free_var(args);
        return make_error_pack(E_INVARG);
    }
    raw = moobinary_to_raw_bytes(args.v.list[1].v.str, &length);
    if (!raw) {
        free_var(args);
        return make_error_pack(E_INVARG);
    }
    stream_add_bytes(s->open, raw, length);
    free_var(args);
    return make_int_pack(length);
}

static package
bound_stream_append_bytes(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    Stream *temporary = new_stream(32);
    enum error e = E_NONE;
    size_t length;
    int i;
    (void) progr;

    if (!s->open)
        e = E_INVARG;
    for (i = 1; e == E_NONE && i <= args.v.list[0].v.num; i++)
        e = append_binary_value(temporary, args.v.list[i]);
    if (e == E_NONE) {
        length = stream_length(temporary);
        stream_add_bytes(s->open, stream_contents(temporary), length);
    }
    free_stream(temporary);
    free_var(args);
    return e == E_NONE ? make_int_pack(length) : make_error_pack(e);
}

static package
bound_stream_append_text(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    Stream *text = new_stream(32), *encoded = new_stream(32);
    const char *encoding;
    enum error e;
    size_t length;
    (void) progr;

    if (!s->open)
        e = E_INVARG;
    else
        e = append_text_value(text, args.v.list[1]);
    encoding = args.v.list[0].v.num == 2 ? args.v.list[2].v.str : s->encoding;
    if (e == E_NONE
        && !stream_add_recoded_chars(encoded, stream_contents(text),
                                     stream_length(text), moo_character_encoding(),
                                     encoding))
        e = E_INVARG;
    if (e == E_NONE) {
        length = stream_length(encoded);
        stream_add_bytes(s->open, stream_contents(encoded), length);
    }
    free_stream(text);
    free_stream(encoded);
    free_var(args);
    return e == E_NONE ? make_int_pack(length) : make_error_pack(e);
}

static package
bound_stream_clear(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    (void) progr;
    free_var(args);
    if (!s->open)
        return make_error_pack(E_INVARG);
    reset_stream(s->open);
    return make_int_pack(0);
}

static package
bound_stream_binary_string(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    Var result = make_binary_string(bound_stream_data(s), bound_stream_length(s));
    (void) progr;
    free_var(args);
    return make_var_pack(result);
}

static package
bound_stream_string(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    const char *encoding = args.v.list[0].v.num ? args.v.list[1].v.str : s->encoding;
    Stream *out = new_stream(bound_stream_length(s) + 1);
    Var result;
    (void) progr;

    if (!stream_add_recoded_chars(out, bound_stream_data(s),
                                  bound_stream_length(s), encoding,
                                  moo_character_encoding())
        || memchr(stream_contents(out), '\0', stream_length(out))) {
        free_stream(out);
        free_var(args);
        return make_error_pack(E_INVARG);
    }
    result.type = TYPE_STR;
    result.v.str = str_dup(stream_contents(out));
    free_stream(out);
    free_var(args);
    return make_var_pack(result);
}

static package
bound_stream_seal(BoundValue *value, Var args, Objid progr)
{
    BoundStream *s = bound_payload(value);
    Var result;
    size_t length;
    char *data;
    (void) progr;
    free_var(args);

    if (s->open) {
        data = stream_detach(s->open, &length);
        s->open = NULL;
        s->sealed = byte_store_new(data, length);
    }
    byte_store_retain(s->sealed);
    result = new_bound_value("bytes", bound_owner(value), s->sealed);
    return make_var_pack(result);
}

static enum error
bound_stream_length_prop(BoundValue *value, Var *out, Objid progr, int write)
{
    BoundStream *s = bound_payload(value);
    (void) progr;
    (void) write;
    out->type = TYPE_INT;
    out->v.num = bound_stream_length(s);
    return E_NONE;
}

static enum error
bound_stream_sealed_prop(BoundValue *value, Var *out, Objid progr, int write)
{
    BoundStream *s = bound_payload(value);
    (void) progr;
    (void) write;
    out->type = TYPE_INT;
    out->v.num = s->open == NULL;
    return E_NONE;
}

static enum error
bound_stream_encode(void *payload, BoundEncoder *out)
{
    BoundStream *s = payload;
    size_t encoding_length = strlen(s->encoding);
    unsigned char header[5];

    if (encoding_length > UINT32_MAX)
        return E_QUOTA;
    header[0] = (encoding_length >> 24) & 0xff;
    header[1] = (encoding_length >> 16) & 0xff;
    header[2] = (encoding_length >> 8) & 0xff;
    header[3] = encoding_length & 0xff;
    header[4] = s->open == NULL;
    if (bound_encoder_write(out, header, sizeof(header)) != E_NONE
        || bound_encoder_write(out, s->encoding, encoding_length) != E_NONE
        || bound_encoder_write(out, bound_stream_data(s),
                               bound_stream_length(s)) != E_NONE)
        return E_QUOTA;
    return E_NONE;
}

static enum error
bound_stream_decode(unsigned schema, BoundDecoder *in, void **payload)
{
    BoundStream *s;
    const unsigned char *raw;
    char *data;
    Stream *encoding;
    size_t length = 0, data_length;
    uint32_t encoding_length;

    raw = bound_decoder_bytes(in, &length);
    if (schema != 1 || bound_decoder_ref_count(in) != 0 || length < 5)
        return E_INVARG;
    encoding_length = ((uint32_t) raw[0] << 24)
        | ((uint32_t) raw[1] << 16) | ((uint32_t) raw[2] << 8) | raw[3];
    if ((size_t) encoding_length > length - 5 || raw[4] > 1)
        return E_INVARG;
    if (memchr(raw + 5, 0, encoding_length))
        return E_INVARG;
    encoding = new_stream((size_t) encoding_length + 1);
    stream_add_bytes(encoding, (const char *) raw + 5, encoding_length);
    data_length = length - 5 - encoding_length;
    s = mymalloc(sizeof(*s), M_BOUND_XTRA);
    s->encoding = str_dup(stream_contents(encoding));
    free_stream(encoding);
    s->sealed = NULL;
    s->open = new_stream(data_length + 1);
    stream_add_bytes(s->open, (const char *) raw + 5 + encoding_length,
                     data_length);
    if (raw[4]) {
        data = stream_detach(s->open, &data_length);
        s->open = NULL;
        s->sealed = byte_store_new(data, data_length);
    }
    *payload = s;
    return E_NONE;
}

static size_t
bound_stream_size(void *payload)
{
    BoundStream *s = payload;
    return sizeof(*s) + bound_stream_length(s) + strlen(s->encoding);
}

static enum error
bound_stream_input(void *payload, const char *data, size_t length, int binary,
                   size_t *written)
{
    BoundStream *s = payload;
    const char *raw = data;
    size_t raw_length = length;
    if (!s->open)
        return E_INVARG;
    if (binary) {
        raw = moobinary_to_raw_bytes(data, &raw_length);
        if (!raw)
            return E_INVARG;
    }
    stream_add_bytes(s->open, raw, raw_length);
    *written = raw_length;
    return E_NONE;
}

static void
free_input_sink_request(void *data)
{
    input_sink_request *request = data;
    free_var(request->sink);
    myfree(request, M_BOUND_XTRA);
}

static package
bound_stream_read(BoundValue *value, Var args, Objid progr)
{
    Objid connection = args.v.list[1].v.obj;
    input_sink_request *request;
    if (!is_wizard(progr)
        && (!valid(connection) || progr != db_object_owner(connection))) {
        free_var(args);
        return make_error_pack(E_PERM);
    }
    request = mymalloc(sizeof(*request), M_BOUND_XTRA);
    request->connection = connection;
    request->sink.type = TYPE_BOUND;
    request->sink.v.bound = value;
    addref(value);
    free_var(args);
    return make_suspend_pack_with_cancel(make_reading_task_into, request,
                                         free_input_sink_request);
}

static const BoundVerbDef bytes_verbs[] = {
    {"binary_string", bytes_binary_string, BOUND_OP_PUBLIC, 0, 0, {TYPE_ANY}},
    {"string", bytes_string, BOUND_OP_PUBLIC, 0, 1, {TYPE_STR}},
    {NULL}
};
static const BoundPropDef bytes_props[] = {
    {"length", bytes_length_prop, BOUND_OP_PUBLIC},
    {NULL}
};
static const BoundVerbDef stream_verbs[] = {
    {"append_text", bound_stream_append_text, 0, 1, 2, {TYPE_ANY, TYPE_STR}},
    {"append_binary", bound_stream_append_binary, 0, 1, 1, {TYPE_STR}},
    {"append_bytes", bound_stream_append_bytes, 0, 0, -1, {TYPE_ANY}},
    {"clear", bound_stream_clear, 0, 0, 0, {TYPE_ANY}},
    {"binary_string", bound_stream_binary_string, BOUND_OP_PUBLIC, 0, 0, {TYPE_ANY}},
    {"string", bound_stream_string, BOUND_OP_PUBLIC, 0, 1, {TYPE_STR}},
    {"seal", bound_stream_seal, 0, 0, 0, {TYPE_ANY}},
    {"read", bound_stream_read, 0, 1, 1, {TYPE_OBJ}},
    {NULL}
};
static const BoundPropDef stream_props[] = {
    {"length", bound_stream_length_prop, BOUND_OP_PUBLIC},
    {"sealed", bound_stream_sealed_prop, BOUND_OP_PUBLIC},
    {NULL}
};
static const BoundTypeDef bytes_type = {
    .name = "bytes",
    .schema_version = 1,
    .verbs = bytes_verbs,
    .properties = bytes_props,
    .construct = bytes_construct,
    .destroy = byte_store_release,
    .encode = bytes_encode,
    .decode = bytes_decode,
    .bytes = bytes_size,
    .equal = bytes_equal,
    .hash = bytes_hash,
    .output = bytes_output,
    .byte_source = bytes_source
};
static const BoundTypeDef stream_type = {
    .name = "stream",
    .schema_version = 1,
    .verbs = stream_verbs,
    .properties = stream_props,
    .construct = bound_stream_construct,
    .destroy = bound_stream_destroy,
    .encode = bound_stream_encode,
    .decode = bound_stream_decode,
    .bytes = bound_stream_size,
    .byte_source = bound_stream_source,
    .input = bound_stream_input
};

void
register_bound_stream_types(void)
{
    if (!register_bound_type(&bytes_type) || !register_bound_type(&stream_type))
        panic("duplicate bound stream type registration");
}


#endif /* BOUND_CORE && BOUND_STREAM */

#if !defined(BOUND_CORE) || !defined(BOUND_STREAM)
void register_bound_stream_types(void) { }
#endif
