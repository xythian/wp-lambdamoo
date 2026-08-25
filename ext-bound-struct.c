#include "bound.h"
#include "ext-bound-stream.h"

#include "config.h"
#include "options.h"

void register_bound_struct_types(void);

#if defined(BOUND_CORE) && defined(BOUND_STREAM) && defined(BOUND_STRUCT)

#include "exceptions.h"
#include "storage.h"
#include "utils.h"

typedef struct StructCursor {
    Var source;
    BoundByteSource bytes;
    size_t start;
    size_t length;
    size_t position;
} StructCursor;

static void
put_u64(unsigned char *out, uint64_t value)
{
    int i;
    for (i = 7; i >= 0; i--) {
        out[i] = value & 0xff;
        value >>= 8;
    }
}

static uint64_t
get_u64(const unsigned char *in)
{
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; i++)
        value = (value << 8) | in[i];
    return value;
}

static void
cursor_destroy(void *payload)
{
    StructCursor *cursor = payload;
    cursor->bytes.release(cursor->bytes.token);
    free_var(cursor->source);
    myfree(cursor, M_BOUND_XTRA);
}

static Var
new_cursor(Objid owner, Var source, BoundByteSource bytes,
           size_t start, size_t length)
{
    StructCursor *cursor = mymalloc(sizeof(*cursor), M_BOUND_XTRA);
    Var result;

    cursor->source = source;
    cursor->bytes = bytes;
    cursor->start = start;
    cursor->length = length;
    cursor->position = 0;
    result = new_bound_value("struct.cursor", owner, cursor);
    if (result.type != TYPE_BOUND) {
        cursor_destroy(cursor);
        result.type = TYPE_ERR;
        result.v.err = E_INVARG;
    }
    return result;
}

static package
cursor_construct(Var args, Objid progr)
{
    int nargs = args.v.list[0].v.num;
    Var source;
    BoundByteSource bytes;
    size_t start = 0, length;
    enum error e;

    if (nargs < 1 || nargs > 3 || args.v.list[1].type != TYPE_BOUND
        || (nargs >= 2 && args.v.list[2].type != TYPE_INT)
        || (nargs >= 3 && args.v.list[3].type != TYPE_INT)) {
        free_var(args);
        return make_error_pack(nargs < 1 || nargs > 3 ? E_ARGS : E_TYPE);
    }
    if ((nargs >= 2 && args.v.list[2].v.num < 0)
        || (nargs >= 3 && args.v.list[3].v.num < 0)) {
        free_var(args);
        return make_error_pack(E_INVARG);
    }
    e = bound_byte_source(args.v.list[1].v.bound, &bytes);
    if (e != E_NONE) {
        free_var(args);
        return make_error_pack(e);
    }
    if (nargs >= 2)
        start = (size_t) args.v.list[2].v.num;
    length = nargs >= 3 ? (size_t) args.v.list[3].v.num
                        : (start <= bytes.length ? bytes.length - start : 0);
    if (start > bytes.length || length > bytes.length - start
        || length > (size_t) NUM_MAX) {
        bytes.release(bytes.token);
        free_var(args);
        return make_error_pack(E_RANGE);
    }
    source = var_ref(args.v.list[1]);
    free_var(args);
    {
        Var result = new_cursor(progr, source, bytes, start, length);
        return result.type == TYPE_ERR ? make_error_pack(result.v.err)
                                       : make_var_pack(result);
    }
}

static enum error
cursor_encode(void *payload, BoundEncoder *out)
{
    StructCursor *cursor = payload;
    unsigned char data[24];

    if (bound_encoder_add_ref(out, cursor->source) == UINT_MAX)
        return E_QUOTA;
    put_u64(data, cursor->start);
    put_u64(data + 8, cursor->length);
    put_u64(data + 16, cursor->position);
    return bound_encoder_write(out, data, sizeof data);
}

static enum error
cursor_decode(unsigned schema, BoundDecoder *in, void **payload)
{
    const unsigned char *data;
    size_t blob_length;
    uint64_t start64, length64, position64;
    Var source;
    BoundByteSource bytes;
    StructCursor *cursor;
    enum error e;

    data = bound_decoder_bytes(in, &blob_length);
    if (schema != 1 || blob_length != 24 || bound_decoder_ref_count(in) != 1)
        return E_INVARG;
    start64 = get_u64(data);
    length64 = get_u64(data + 8);
    position64 = get_u64(data + 16);
    if (start64 > (uint64_t) (size_t) -1
        || length64 > (uint64_t) (size_t) -1
        || position64 > (uint64_t) (size_t) -1
        || length64 > (uint64_t) NUM_MAX
        || position64 > (uint64_t) NUM_MAX)
        return E_RANGE;
    e = bound_decoder_ref(in, 0, &source);
    if (e != E_NONE)
        return e;
    if (source.type != TYPE_BOUND) {
        free_var(source);
        return E_TYPE;
    }
    e = bound_byte_source(source.v.bound, &bytes);
    if (e != E_NONE) {
        free_var(source);
        return e;
    }
    if ((size_t) start64 > bytes.length
        || (size_t) length64 > bytes.length - (size_t) start64
        || (size_t) position64 > (size_t) length64) {
        bytes.release(bytes.token);
        free_var(source);
        return E_RANGE;
    }
    cursor = mymalloc(sizeof(*cursor), M_BOUND_XTRA);
    cursor->source = source;
    cursor->bytes = bytes;
    cursor->start = (size_t) start64;
    cursor->length = (size_t) length64;
    cursor->position = (size_t) position64;
    *payload = cursor;
    return E_NONE;
}

static size_t
cursor_bytes(void *payload)
{
    (void) payload;
    return sizeof(StructCursor);
}

static enum error
cursor_property(BoundValue *value, Var *out, Objid progr, int write,
                int which)
{
    StructCursor *cursor = bound_payload(value);
    (void) progr;
    if (write)
        return E_PERM;
    out->type = TYPE_INT;
    if (which == 0)
        out->v.num = cursor->position;
    else if (which == 1)
        out->v.num = cursor->length;
    else
        out->v.num = cursor->length - cursor->position;
    return E_NONE;
}

static enum error
cursor_position_prop(BoundValue *value, Var *out, Objid progr, int write)
{
    return cursor_property(value, out, progr, write, 0);
}

static enum error
cursor_length_prop(BoundValue *value, Var *out, Objid progr, int write)
{
    return cursor_property(value, out, progr, write, 1);
}

static enum error
cursor_remaining_prop(BoundValue *value, Var *out, Objid progr, int write)
{
    return cursor_property(value, out, progr, write, 2);
}

static enum error
cursor_read(StructCursor *cursor, void *out, size_t length)
{
    enum error e;
    if (length > cursor->length - cursor->position)
        return E_RANGE;
    e = cursor->bytes.read_at(cursor->bytes.token,
                              cursor->start + cursor->position, out, length);
    if (e == E_NONE)
        cursor->position += length;
    return e;
}

static package
cursor_seek(BoundValue *value, Var args, Objid progr)
{
    StructCursor *cursor = bound_payload(value);
    Num position = args.v.list[1].v.num;
    (void) progr;
    free_var(args);
    if (position < 0 || (UNum) position > cursor->length)
        return make_error_pack(E_RANGE);
    cursor->position = position;
    return make_int_pack(position);
}

static package
cursor_skip(BoundValue *value, Var args, Objid progr)
{
    StructCursor *cursor = bound_payload(value);
    Num amount = args.v.list[1].v.num;
    size_t position;
    (void) progr;
    free_var(args);
    if (amount < 0) {
        UNum magnitude = (UNum) (-(amount + 1)) + 1;
        if (magnitude > cursor->position)
            return make_error_pack(E_RANGE);
        position = cursor->position - (size_t) magnitude;
    } else {
        if ((UNum) amount > cursor->length - cursor->position)
            return make_error_pack(E_RANGE);
        position = cursor->position + (size_t) amount;
    }
    cursor->position = position;
    return make_int_pack(position);
}

static package
cursor_align(BoundValue *value, Var args, Objid progr)
{
    StructCursor *cursor = bound_payload(value);
    Num alignment = args.v.list[1].v.num;
    size_t remainder, advance;
    (void) progr;
    free_var(args);
    if (alignment <= 0)
        return make_error_pack(E_INVARG);
    remainder = cursor->position % (size_t) alignment;
    advance = remainder ? (size_t) alignment - remainder : 0;
    if (advance > cursor->length - cursor->position)
        return make_error_pack(E_RANGE);
    cursor->position += advance;
    return make_int_pack(cursor->position);
}

static uint64_t
decode_uint(const unsigned char *data, size_t width, int little)
{
    uint64_t result = 0;
    size_t i;
    if (little) {
        for (i = width; i > 0; i--)
            result = (result << 8) | data[i - 1];
    } else {
        for (i = 0; i < width; i++)
            result = (result << 8) | data[i];
    }
    return result;
}

static int
parse_integer_codec(const char *name, size_t *width, int *is_signed,
                    int *little)
{
    char sign = name[0];
    char width_char = name[1];
    const char *suffix = name + 2;

    if ((sign != 'u' && sign != 'i') || width_char < '1' || width_char > '8'
        || width_char == '3' || width_char == '5' || width_char == '6'
        || width_char == '7')
        return 0;
    *width = width_char - '0';
    *is_signed = sign == 'i';
    if (*width == 1) {
        if (*suffix)
            return 0;
        *little = 0;
    } else if (!mystrcasecmp(suffix, "le"))
        *little = 1;
    else if (!mystrcasecmp(suffix, "be"))
        *little = 0;
    else
        return 0;
    return 1;
}

static package
cursor_read_codec(BoundValue *value, Var args, Objid progr)
{
    StructCursor *cursor = bound_payload(value);
    const char *codec = args.v.list[1].v.str;
    unsigned char data[8];
    size_t width;
    int is_signed, little;
    enum error e;
    uint64_t raw;
    Var result;
    (void) progr;

    if (parse_integer_codec(codec, &width, &is_signed, &little)) {
        free_var(args);
        e = cursor_read(cursor, data, width);
        if (e != E_NONE)
            return make_error_pack(e);
        raw = decode_uint(data, width, little);
        if (is_signed && (raw & ((uint64_t) 1 << (width * 8 - 1)))) {
            int64_t signed_value = width == 8
                ? -1 - (int64_t) (UINT64_MAX - raw)
                : (int64_t) raw - ((int64_t) 1 << (width * 8));
            if (signed_value < (int64_t) NUM_MIN
                || signed_value > (int64_t) NUM_MAX) {
                cursor->position -= width;
                return make_error_pack(E_RANGE);
            }
            result.v.num = (Num) signed_value;
        } else {
            if (raw > (uint64_t) NUM_MAX) {
                cursor->position -= width;
                return make_error_pack(E_RANGE);
            }
            result.v.num = (Num) raw;
        }
        result.type = TYPE_INT;
        return make_var_pack(result);
    }
    if (!mystrcasecmp(codec, "f4be") || !mystrcasecmp(codec, "f4le")) {
        uint32_t bits;
        float number;
        little = !mystrcasecmp(codec, "f4le");
        free_var(args);
        e = cursor_read(cursor, data, 4);
        if (e != E_NONE)
            return make_error_pack(e);
        bits = (uint32_t) decode_uint(data, 4, little);
        memcpy(&number, &bits, sizeof number);
        return make_float_pack((FlNum) number);
    }
    if (!mystrcasecmp(codec, "f8be") || !mystrcasecmp(codec, "f8le")) {
        uint64_t bits;
        double number;
        little = !mystrcasecmp(codec, "f8le");
        free_var(args);
        e = cursor_read(cursor, data, 8);
        if (e != E_NONE)
            return make_error_pack(e);
        bits = decode_uint(data, 8, little);
        memcpy(&number, &bits, sizeof number);
        return make_float_pack((FlNum) number);
    }
    free_var(args);
    return make_error_pack(E_INVARG);
}

static package
cursor_read_bytes(BoundValue *value, Var args, Objid progr)
{
    StructCursor *cursor = bound_payload(value);
    Num requested = args.v.list[1].v.num;
    char *data;
    Var result;
    enum error e;

    (void) progr;
    free_var(args);
    if (requested < 0)
        return make_error_pack(E_INVARG);
    if ((UNum) requested > cursor->length - cursor->position)
        return make_error_pack(E_RANGE);
    data = mymalloc((size_t) requested + 1, M_STREAM);
    e = cursor->bytes.read_at(cursor->bytes.token,
                              cursor->start + cursor->position,
                              data, (size_t) requested);
    if (e != E_NONE) {
        myfree(data, M_STREAM);
        return make_error_pack(e);
    }
    data[requested] = 0;
    result = make_bound_bytes_take(bound_owner(value), data,
                                   (size_t) requested, NULL, NULL);
    if (result.type != TYPE_BOUND)
        return make_error_pack(result.type == TYPE_ERR ? result.v.err : E_INVARG);
    cursor->position += (size_t) requested;
    return make_var_pack(result);
}

static package
cursor_subcursor(BoundValue *value, Var args, Objid progr)
{
    StructCursor *cursor = bound_payload(value);
    Num requested = args.v.list[1].v.num;
    BoundByteSource bytes;
    Var source, result;
    enum error e;

    (void) progr;
    free_var(args);
    if (requested < 0)
        return make_error_pack(E_INVARG);
    if ((UNum) requested > cursor->length - cursor->position)
        return make_error_pack(E_RANGE);
    e = bound_byte_source(cursor->source.v.bound, &bytes);
    if (e != E_NONE)
        return make_error_pack(e);
    source = var_ref(cursor->source);
    result = new_cursor(bound_owner(value), source, bytes,
                        cursor->start + cursor->position, (size_t) requested);
    if (result.type != TYPE_BOUND)
        return make_error_pack(result.type == TYPE_ERR ? result.v.err : E_INVARG);
    cursor->position += (size_t) requested;
    return make_var_pack(result);
}

static const BoundVerbDef cursor_verbs[] = {
    {"seek", cursor_seek, 0, 1, 1, {TYPE_INT}},
    {"skip", cursor_skip, 0, 1, 1, {TYPE_INT}},
    {"align", cursor_align, 0, 1, 1, {TYPE_INT}},
    {"read", cursor_read_codec, 0, 1, 1, {TYPE_STR}},
    {"bytes", cursor_read_bytes, 0, 1, 1, {TYPE_INT}},
    {"subcursor", cursor_subcursor, 0, 1, 1, {TYPE_INT}},
    {NULL}
};

static const BoundPropDef cursor_props[] = {
    {"position", cursor_position_prop, BOUND_OP_PUBLIC},
    {"length", cursor_length_prop, BOUND_OP_PUBLIC},
    {"remaining", cursor_remaining_prop, BOUND_OP_PUBLIC},
    {NULL}
};

static const BoundTypeDef cursor_type = {
    .name = "struct.cursor",
    .schema_version = 1,
    .verbs = cursor_verbs,
    .properties = cursor_props,
    .construct = cursor_construct,
    .destroy = cursor_destroy,
    .encode = cursor_encode,
    .decode = cursor_decode,
    .bytes = cursor_bytes
};

void
register_bound_struct_types(void)
{
    if (!register_bound_type(&cursor_type))
        panic("duplicate bound struct type registration");
}

#else

void
register_bound_struct_types(void)
{
}

#endif /* BOUND_CORE && BOUND_STREAM && BOUND_STRUCT */
