#include "protocol.h"

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

int
sp_encode_hello(const struct sp_hello *hello, unsigned char **payload,
                uint32_t *length)
{
    unsigned char *p;
    uint32_t total = SP_HELLO_FIXED_SIZE + hello->origin_length;

    if (total > SP_MAX_CONTROL)
        return -1;
    p = calloc(1, total);
    if (!p)
        return -1;

    memcpy(p, hello->session_id, SP_ID_SIZE);
    memcpy(p + 16, hello->edge_id, SP_ID_SIZE);
    sp_put_u64(p + 32, hello->generation);
    p[40] = hello->mode;
    sp_put_u64(p + 48, hello->input_position);
    sp_put_u64(p + 56, hello->output_position);
    put_u16(p + 64, hello->origin_length);
    if (hello->origin_length)
        memcpy(p + SP_HELLO_FIXED_SIZE, hello->origin, hello->origin_length);
    *payload = p;
    *length = total;
    return 0;
}

int
sp_decode_hello(const struct sp_frame *frame, struct sp_hello *hello)
{
    uint16_t origin_length;

    if (frame->type != SP_HELLO || frame->length < SP_HELLO_FIXED_SIZE)
        return -1;
    origin_length = get_u16(frame->payload + 64);
    if ((uint32_t) SP_HELLO_FIXED_SIZE + origin_length != frame->length
        || !all_zero(frame->payload + 41, 7))
        return -1;

    memset(hello, 0, sizeof(*hello));
    memcpy(hello->session_id, frame->payload, SP_ID_SIZE);
    memcpy(hello->edge_id, frame->payload + 16, SP_ID_SIZE);
    hello->generation = sp_get_u64(frame->payload + 32);
    hello->mode = frame->payload[40];
    if (hello->mode < SP_MODE_INITIAL || hello->mode > SP_MODE_CRASH)
        return -1;
    hello->input_position = sp_get_u64(frame->payload + 48);
    hello->output_position = sp_get_u64(frame->payload + 56);
    hello->origin_length = origin_length;
    hello->origin = frame->payload + SP_HELLO_FIXED_SIZE;
    return 0;
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
    return detach->mode == SP_MODE_GRACEFUL || detach->mode == SP_MODE_CRASH
        ? 0 : -1;
}
