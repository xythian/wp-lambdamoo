#include "session_protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint16_t
get_u16(const unsigned char *p)
{
    return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
}

static uint32_t
get_u32(const unsigned char *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
        | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

static void
put_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char) (value >> 8);
    p[1] = (unsigned char) value;
}

static void
put_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char) (value >> 24);
    p[1] = (unsigned char) (value >> 16);
    p[2] = (unsigned char) (value >> 8);
    p[3] = (unsigned char) value;
}

uint64_t
sp_get_u64(const unsigned char *p)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < 8; i++)
        value = (value << 8) | p[i];
    return value;
}

void
sp_put_u64(unsigned char *p, uint64_t value)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (unsigned char) value;
        value >>= 8;
    }
}

static int
all_zero(const unsigned char *p, size_t length)
{
    size_t i;

    for (i = 0; i < length; i++)
        if (p[i] != 0)
            return 0;
    return 1;
}

static int
read_full(int fd, void *buffer, size_t length)
{
    unsigned char *p = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count = read(fd, p + done, length - done);
        if (count > 0)
            done += (size_t) count;
        else if (count == 0)
            return done == 0 ? 0 : -2;
        else if (errno != EINTR)
            return -1;
    }
    return 1;
}

static int
write_full(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count = write(fd, p + done, length - done);
        if (count > 0)
            done += (size_t) count;
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return -1;
    }
    return 1;
}

static int
valid_type_and_length(uint8_t type, uint32_t length)
{
    switch (type) {
    case SP_HELLO:
        return length >= SP_HELLO_FIXED_SIZE && length <= SP_MAX_CONTROL;
    case SP_WELCOME:
        return length == SP_WELCOME_SIZE;
    case SP_INPUT:
    case SP_OUTPUT:
        return length >= 8 && length <= SP_MAX_PAYLOAD;
    case SP_ACK:
        return length == SP_ACK_SIZE;
    case SP_DETACH:
        return length == SP_DETACH_SIZE;
    case SP_CLOSE:
    case SP_ERROR:
        return length <= SP_MAX_CONTROL;
    case SP_BIND:
        return length == SP_BIND_SIZE;
    case SP_LISTENERS:
        return length >= SP_LISTENERS_FIXED_SIZE && length <= SP_MAX_CONTROL;
    case SP_CONNECT:
        return length > SP_CONNECT_FIXED_SIZE && length <= SP_MAX_CONTROL;
    case SP_CONNECT_FAILED:
        return length >= SP_CONNECT_FAILED_FIXED_SIZE
            && length <= SP_MAX_CONTROL;
    case SP_LISTEN_FAILED:
        return length >= SP_LISTEN_FAILED_FIXED_SIZE
            && length <= SP_MAX_CONTROL;
    default:
        return 0;
    }
}

int
sp_frame_read(int fd, struct sp_frame *frame)
{
    unsigned char header[SP_HEADER_SIZE];
    int result;

    memset(frame, 0, sizeof(*frame));
    result = read_full(fd, header, sizeof(header));
    if (result <= 0)
        return result;
    if (get_u32(header) != SP_MAGIC
        || header[4] != SP_VERSION_MAJOR
        || header[5] > SP_VERSION_MINOR)
        return -2;

    if (header[7] != 0 || get_u16(header + 8) != 0)
        return -2;
    frame->type = header[6];
    frame->flags = 0;
    frame->length = get_u32(header + 10);
    if (!valid_type_and_length(frame->type, frame->length))
        return -2;

    if (frame->length != 0) {
        frame->payload = malloc(frame->length);
        if (!frame->payload)
            return -1;
        result = read_full(fd, frame->payload, frame->length);
        if (result != 1) {
            sp_frame_clear(frame);
            return result == 0 ? -2 : result;
        }
    }
    return 1;
}

int
sp_frame_write(int fd, uint8_t type, uint16_t flags,
               const void *payload, uint32_t length)
{
    unsigned char header[SP_HEADER_SIZE];

    if (flags != 0 || !valid_type_and_length(type, length)
        || (length != 0 && payload == NULL)) {
        errno = EINVAL;
        return -1;
    }

    put_u32(header, SP_MAGIC);
    header[4] = SP_VERSION_MAJOR;
    header[5] = SP_VERSION_MINOR;
    header[6] = type;
    header[7] = 0;
    put_u16(header + 8, flags);
    put_u32(header + 10, length);

    if (write_full(fd, header, sizeof(header)) < 0)
        return -1;
    if (length != 0 && write_full(fd, payload, length) < 0)
        return -1;
    return 1;
}

void
sp_frame_clear(struct sp_frame *frame)
{
    free(frame->payload);
    memset(frame, 0, sizeof(*frame));
}

static int
field_name_ok(const unsigned char *name, size_t length)
{
    size_t i;

    if (length == 0 || length > SP_MAX_FIELD_NAME)
        return 0;
    for (i = 0; i < length; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z')
              || (name[i] >= '0' && name[i] <= '9') || name[i] == '-'))
            return 0;
    return 1;
}

int
sp_next_field(const unsigned char **cursor, const unsigned char *end,
              struct sp_field *field)
{
    const unsigned char *p = *cursor;
    size_t name_length;

    if (p == end)
        return 0;
    name_length = *p++;
    if ((size_t) (end - p) < name_length + 1 || !field_name_ok(p, name_length))
        return -1;
    memset(field, 0, sizeof(*field));
    memcpy(field->name, p, name_length);
    p += name_length;
    field->type = *p++;
    switch (field->type) {
    case SP_FIELD_STRING:
        if (end - p < 2)
            return -1;
        field->string_length = get_u16(p);
        p += 2;
        if ((size_t) (end - p) < field->string_length)
            return -1;
        field->string = p;
        p += field->string_length;
        break;
    case SP_FIELD_INTEGER:
        if (end - p < 8)
            return -1;
        field->integer = (int64_t) sp_get_u64(p);
        p += 8;
        break;
    default:
        return -1;
    }
    *cursor = p;
    return 1;
}

static size_t
put_field_name(unsigned char *p, const char *name, uint8_t type)
{
    size_t length = strlen(name);

    if (p) {
        p[0] = (unsigned char) length;
        memcpy(p + 1, name, length);
        p[1 + length] = type;
    }
    return length + 2;
}

size_t
sp_put_string_field(unsigned char *p, const char *name, const void *value,
                    uint16_t length)
{
    size_t n = put_field_name(p, name, SP_FIELD_STRING);

    if (p) {
        put_u16(p + n, length);
        memcpy(p + n + 2, value, length);
    }
    return n + 2 + length;
}

size_t
sp_put_integer_field(unsigned char *p, const char *name, int64_t value)
{
    size_t n = put_field_name(p, name, SP_FIELD_INTEGER);

    if (p)
        sp_put_u64(p + n, (uint64_t) value);
    return n + 8;
}

/* The fields a client or outbound session must describe itself with, and
 * the type each must have.
 */
static const struct {
    const char *name;
    uint8_t type;
} required_fields[] = {
    {"transport", SP_FIELD_STRING},
    {"local-port", SP_FIELD_INTEGER},
    {"remote-address", SP_FIELD_STRING},
    {"remote-port", SP_FIELD_INTEGER},
};

#define REQUIRED_FIELDS (sizeof(required_fields) / sizeof(required_fields[0]))

static int
check_fields(const struct sp_hello *hello)
{
    const unsigned char *cursor = hello->fields;
    const unsigned char *end = hello->fields + hello->fields_length;
    char names[SP_MAX_FIELDS][SP_MAX_FIELD_NAME + 1];
    unsigned count = 0, found = 0, i;
    struct sp_field field;
    int r;

    while ((r = sp_next_field(&cursor, end, &field)) > 0) {
        if (count == SP_MAX_FIELDS || !strcmp(field.name, "direction"))
            return -1;
        for (i = 0; i < count; i++)
            if (!strcmp(names[i], field.name))
                return -1;
        strcpy(names[count++], field.name);
        if ((!strcmp(field.name, "local-port")
             || !strcmp(field.name, "remote-port"))
            && (field.integer < 0 || field.integer > 65535))
            return -1;
        for (i = 0; i < REQUIRED_FIELDS; i++)
            if (!strcmp(field.name, required_fields[i].name)) {
                if (field.type != required_fields[i].type)
                    return -1;
                found++;
            }
    }
    if (r < 0)
        return -1;
    if (hello->kind == SP_KIND_CONTROL)
        return count == 0 ? 0 : -1;
    return found == REQUIRED_FIELDS ? 0 : -1;
}

int
sp_encode_hello(const struct sp_hello *hello, unsigned char **payload,
                uint32_t *length)
{
    unsigned char *p;
    uint32_t total = SP_HELLO_FIXED_SIZE + hello->fields_length;

    if (total > SP_MAX_CONTROL)
        return -1;
    p = calloc(1, total);
    if (!p)
        return -1;

    memcpy(p, hello->session_id, SP_ID_SIZE);
    memcpy(p + 16, hello->edge_id, SP_ID_SIZE);
    sp_put_u64(p + 32, hello->generation);
    p[40] = hello->mode;
    p[41] = hello->kind;
    put_u16(p + 42, hello->listen_port);
    put_u32(p + 44, hello->request);
    sp_put_u64(p + 48, hello->input_position);
    sp_put_u64(p + 56, hello->output_position);
    sp_put_u64(p + 64, (uint64_t) hello->player);
    sp_put_u64(p + 72, (uint64_t) hello->listener);
    memcpy(p + 80, hello->database, SP_DATABASE_ID_SIZE);
    put_u16(p + 96, hello->fields_length);
    if (hello->fields_length)
        memcpy(p + SP_HELLO_FIXED_SIZE, hello->fields, hello->fields_length);
    *payload = p;
    *length = total;
    return 0;
}

int
sp_decode_hello(const struct sp_frame *frame, struct sp_hello *hello)
{
    uint16_t fields_length;

    if (frame->type != SP_HELLO || frame->length < SP_HELLO_FIXED_SIZE)
        return -1;
    fields_length = get_u16(frame->payload + 96);
    if ((uint32_t) SP_HELLO_FIXED_SIZE + fields_length != frame->length)
        return -1;

    memset(hello, 0, sizeof(*hello));
    memcpy(hello->session_id, frame->payload, SP_ID_SIZE);
    memcpy(hello->edge_id, frame->payload + 16, SP_ID_SIZE);
    hello->generation = sp_get_u64(frame->payload + 32);
    hello->mode = frame->payload[40];
    if (hello->mode < SP_MODE_INITIAL || hello->mode > SP_MODE_CRASH)
        return -1;
    if (hello->mode == SP_MODE_INITIAL
        && !all_zero(frame->payload + 80, SP_DATABASE_ID_SIZE))
        return -1;
    memcpy(hello->database, frame->payload + 80, SP_DATABASE_ID_SIZE);
    hello->kind = frame->payload[41];
    hello->listen_port = get_u16(frame->payload + 42);
    hello->request = get_u32(frame->payload + 44);
    switch (hello->kind) {
    case SP_KIND_CLIENT:
        if (hello->request != 0)
            return -1;
        break;
    case SP_KIND_OUTBOUND:
        if (hello->listen_port != 0
            || (hello->mode == SP_MODE_INITIAL) != (hello->request != 0))
            return -1;
        break;
    case SP_KIND_CONTROL:
        if (hello->listen_port != 0 || hello->request != 0
            || hello->mode != SP_MODE_INITIAL)
            return -1;
        break;
    default:
        return -1;
    }
    hello->input_position = sp_get_u64(frame->payload + 48);
    hello->output_position = sp_get_u64(frame->payload + 56);
    hello->fields_length = fields_length;
    hello->player = (int64_t) sp_get_u64(frame->payload + 64);
    hello->listener = (int64_t) sp_get_u64(frame->payload + 72);
    hello->fields = frame->payload + SP_HELLO_FIXED_SIZE;
    return check_fields(hello);
}

void
sp_encode_welcome(const struct sp_welcome *welcome,
                  unsigned char payload[SP_WELCOME_SIZE])
{
    memset(payload, 0, SP_WELCOME_SIZE);
    sp_put_u64(payload, welcome->generation);
    payload[8] = welcome->mode;
    sp_put_u64(payload + 16, welcome->input_position);
    sp_put_u64(payload + 24, welcome->output_position);
}

int
sp_decode_welcome(const struct sp_frame *frame, struct sp_welcome *welcome)
{
    if (frame->type != SP_WELCOME || frame->length != SP_WELCOME_SIZE
        || !all_zero(frame->payload + 9, 7))
        return -1;
    welcome->generation = sp_get_u64(frame->payload);
    welcome->mode = frame->payload[8];
    welcome->input_position = sp_get_u64(frame->payload + 16);
    welcome->output_position = sp_get_u64(frame->payload + 24);
    return welcome->mode >= SP_MODE_INITIAL && welcome->mode <= SP_MODE_CRASH
        ? 0 : -1;
}

void
sp_encode_ack(const struct sp_ack *ack, unsigned char payload[SP_ACK_SIZE])
{
    memset(payload, 0, SP_ACK_SIZE);
    payload[0] = ack->direction;
    sp_put_u64(payload + 8, ack->position);
}

int
sp_decode_ack(const struct sp_frame *frame, struct sp_ack *ack)
{
    if (frame->type != SP_ACK || frame->length != SP_ACK_SIZE
        || !all_zero(frame->payload + 1, 7))
        return -1;
    ack->direction = frame->payload[0];
    ack->position = sp_get_u64(frame->payload + 8);
    return ack->direction == SP_DIR_INPUT || ack->direction == SP_DIR_OUTPUT
        ? 0 : -1;
}

void
sp_encode_detach(const struct sp_detach *detach,
                 unsigned char payload[SP_DETACH_SIZE])
{
    memset(payload, 0, SP_DETACH_SIZE);
    payload[0] = detach->mode;
    sp_put_u64(payload + 8, detach->input_position);
    sp_put_u64(payload + 16, detach->output_position);
    memcpy(payload + 24, detach->database, SP_DATABASE_ID_SIZE);
}

int
sp_decode_detach(const struct sp_frame *frame, struct sp_detach *detach)
{
    if (frame->type != SP_DETACH || frame->length != SP_DETACH_SIZE
        || !all_zero(frame->payload + 1, 7))
        return -1;
    detach->mode = frame->payload[0];
    detach->input_position = sp_get_u64(frame->payload + 8);
    detach->output_position = sp_get_u64(frame->payload + 16);
    memcpy(detach->database, frame->payload + 24, SP_DATABASE_ID_SIZE);
    return detach->mode == SP_MODE_GRACEFUL || detach->mode == SP_MODE_CRASH
        ? 0 : -1;
}

void
sp_encode_bind(const struct sp_bind *bind, unsigned char payload[SP_BIND_SIZE])
{
    sp_put_u64(payload, (uint64_t) bind->player);
    sp_put_u64(payload + 8, (uint64_t) bind->listener);
}

int
sp_decode_bind(const struct sp_frame *frame, struct sp_bind *bind)
{
    if (frame->type != SP_BIND || frame->length != SP_BIND_SIZE)
        return -1;
    bind->player = (int64_t) sp_get_u64(frame->payload);
    bind->listener = (int64_t) sp_get_u64(frame->payload + 8);
    return 0;
}

int
sp_encode_listeners(uint32_t serial, const struct sp_listen *listens,
                    unsigned count, unsigned char **payload, uint32_t *length)
{
    uint32_t total = SP_LISTENERS_FIXED_SIZE;
    unsigned char *p, *q;
    unsigned i;

    if (count > SP_MAX_LISTENERS)
        return -1;
    for (i = 0; i < count; i++) {
        size_t certificate = listens[i].certificate
            ? strlen(listens[i].certificate) : 0;
        size_t key = listens[i].key ? strlen(listens[i].key) : 0;
        size_t acme = listens[i].acme ? strlen(listens[i].acme) : 0;

        if (certificate > 0xffff || key > 0xffff || acme > 0xffff)
            return -1;
        total += SP_LISTEN_FIXED_SIZE + (uint32_t) (certificate + key + acme);
        if (total > SP_MAX_CONTROL)
            return -1;
    }
    if (serial == 0 || !(p = malloc(total)))
        return -1;
    put_u32(p, serial);
    for (i = 0, q = p + SP_LISTENERS_FIXED_SIZE; i < count; i++) {
        uint16_t certificate = (uint16_t) (listens[i].certificate
            ? strlen(listens[i].certificate) : 0);
        uint16_t key = (uint16_t) (listens[i].key
            ? strlen(listens[i].key) : 0);
        uint16_t acme = (uint16_t) (listens[i].acme
            ? strlen(listens[i].acme) : 0);

        put_u16(q, listens[i].port);
        put_u16(q + 2, listens[i].flags);
        put_u16(q + 4, certificate);
        put_u16(q + 6, key);
        put_u16(q + 8, acme);
        q += SP_LISTEN_FIXED_SIZE;
        if (certificate)
            memcpy(q, listens[i].certificate, certificate);
        q += certificate;
        if (key)
            memcpy(q, listens[i].key, key);
        q += key;
        if (acme)
            memcpy(q, listens[i].acme, acme);
        q += acme;
    }
    *payload = p;
    *length = total;
    return 0;
}

int
sp_encode_connect(const struct sp_connect *connect,
                  unsigned char **payload, uint32_t *length)
{
    uint32_t total = SP_CONNECT_FIXED_SIZE + connect->host_length;
    unsigned char *p;

    if (connect->host_length == 0 || total > SP_MAX_CONTROL)
        return -1;
    p = calloc(1, total);
    if (!p)
        return -1;
    put_u32(p, connect->request);
    put_u16(p + 4, connect->port);
    put_u16(p + 6, connect->flags);
    put_u32(p + 8, connect->timeout);
    put_u16(p + 12, connect->host_length);
    memcpy(p + SP_CONNECT_FIXED_SIZE, connect->host, connect->host_length);
    *payload = p;
    *length = total;
    return 0;
}

int
sp_decode_connect(const struct sp_frame *frame, struct sp_connect *connect)
{
    if (frame->type != SP_CONNECT || frame->length <= SP_CONNECT_FIXED_SIZE)
        return -1;
    memset(connect, 0, sizeof(*connect));
    connect->request = get_u32(frame->payload);
    connect->port = get_u16(frame->payload + 4);
    connect->flags = get_u16(frame->payload + 6);
    connect->timeout = get_u32(frame->payload + 8);
    connect->host_length = get_u16(frame->payload + 12);
    if (connect->request == 0 || (connect->flags & ~SP_CONNECT_TLS)
        || !all_zero(frame->payload + 14, 2)
        || (uint32_t) SP_CONNECT_FIXED_SIZE + connect->host_length
           != frame->length)
        return -1;
    connect->host = frame->payload + SP_CONNECT_FIXED_SIZE;
    return 0;
}

int
sp_encode_connect_failed(const struct sp_connect_failed *failed,
                         unsigned char **payload, uint32_t *length)
{
    uint32_t total = SP_CONNECT_FAILED_FIXED_SIZE + failed->reason_length;
    unsigned char *p;

    if (total > SP_MAX_CONTROL)
        return -1;
    p = calloc(1, total);
    if (!p)
        return -1;
    put_u32(p, failed->request);
    p[4] = failed->code;
    if (failed->reason_length)
        memcpy(p + SP_CONNECT_FAILED_FIXED_SIZE, failed->reason,
               failed->reason_length);
    *payload = p;
    *length = total;
    return 0;
}

int
sp_decode_connect_failed(const struct sp_frame *frame,
                         struct sp_connect_failed *failed)
{
    if (frame->type != SP_CONNECT_FAILED
        || frame->length < SP_CONNECT_FAILED_FIXED_SIZE
        || !all_zero(frame->payload + 5, 3))
        return -1;
    memset(failed, 0, sizeof(*failed));
    failed->request = get_u32(frame->payload);
    failed->code = frame->payload[4];
    if (failed->request == 0 || failed->code < SP_FAILED_DESTINATION
        || failed->code > SP_FAILED_RESOURCES)
        return -1;
    failed->reason_length = frame->length - SP_CONNECT_FAILED_FIXED_SIZE;
    failed->reason = frame->payload + SP_CONNECT_FAILED_FIXED_SIZE;
    return 0;
}

int
sp_encode_listen_failed(const struct sp_listen_failed *failed,
                        unsigned char **payload, uint32_t *length)
{
    uint32_t total = SP_LISTEN_FAILED_FIXED_SIZE + failed->reason_length;
    unsigned char *p;

    if (total > SP_MAX_CONTROL)
        return -1;
    p = calloc(1, total);
    if (!p)
        return -1;
    put_u16(p, failed->port);
    p[2] = failed->code;
    put_u32(p + 4, failed->serial);
    if (failed->reason_length)
        memcpy(p + SP_LISTEN_FAILED_FIXED_SIZE, failed->reason,
               failed->reason_length);
    *payload = p;
    *length = total;
    return 0;
}

int
sp_decode_listen_failed(const struct sp_frame *frame,
                        struct sp_listen_failed *failed)
{
    if (frame->type != SP_LISTEN_FAILED
        || frame->length < SP_LISTEN_FAILED_FIXED_SIZE
        || frame->payload[3] != 0)
        return -1;
    memset(failed, 0, sizeof(*failed));
    failed->port = get_u16(frame->payload);
    failed->code = frame->payload[2];
    failed->serial = get_u32(frame->payload + 4);
    if (failed->port == 0 || failed->serial == 0
        || failed->code < SP_LISTEN_FAILED_PORT
        || failed->code > SP_LISTEN_FAILED_CERTIFICATE)
        return -1;
    failed->reason_length = frame->length - SP_LISTEN_FAILED_FIXED_SIZE;
    failed->reason = frame->payload + SP_LISTEN_FAILED_FIXED_SIZE;
    return 0;
}
