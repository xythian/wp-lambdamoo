/*
 * Connection-independent session network backend.
 *
 * This is selected with NETWORK_PROTOCOL == NP_SESSION.  A stable edge owns
 * each external connection and opens one authenticated Unix stream per MOO
 * session attachment.
 */

#define _GNU_SOURCE 1

#include "config.h"
#include "options.h"

#include "my-ctype.h"
#include "my-fcntl.h"
#include "my-socket.h"
#include "my-signal.h"
#include "my-stat.h"
#include "my-unistd.h"

#include "log.h"
#include "net_mplex.h"
#include "network.h"
#include "server.h"
#include "streams.h"
#include "structures.h"
#include "utf.h"
#include "utf-ctype.h"
#include "utils.h"

#include "prototype/session/protocol.c"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef struct session_output {
    struct session_output *next;
    unsigned char *buffer;
    size_t length;
    size_t offset;
    size_t data_length;
} session_output;

typedef struct session_handle {
    struct session_handle *next, **prev;
    int fd;
    server_handle shandle;
    int attached;
    int input_suspended;
    int binary;
    int last_input_was_CR;
    unsigned char excess_utf[4];
    int excess_utf_count;
    Stream *input;
    char *name;

    unsigned char header[SP_HEADER_SIZE];
    size_t header_used;
    unsigned char *payload;
    uint32_t payload_length;
    size_t payload_used;
    uint8_t frame_type;

    session_output *output_head;
    session_output **output_tail;
    size_t output_length;
    uint64_t input_position;
    uint64_t output_position;
    uint64_t output_acked;
} session_handle;

typedef struct session_listener {
    int fd;
    server_listener slistener;
    char *path;
    const char *name;
} session_listener;

static session_listener *the_listener;
static session_handle *all_handles;

static int
set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | NONBLOCK_FLAG) >= 0;
}

static int
peer_is_allowed(int fd)
{
#ifdef SO_PEERCRED
    struct ucred credentials;
    socklen_t length = sizeof(credentials);

    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) == 0
        && credentials.uid == geteuid();
#else
    (void) fd;
    return 1;
#endif
}

static int
queue_frame(session_handle *h, uint8_t type, const void *payload,
            uint32_t length, size_t data_length)
{
    session_output *output;
    unsigned char *buffer;

    if (!valid_type_and_length(type, length))
        return 0;
    output = malloc(sizeof(*output));
    buffer = malloc(SP_HEADER_SIZE + length);
    if (!output || !buffer) {
        free(output);
        free(buffer);
        return 0;
    }

    put_u32(buffer, SP_MAGIC);
    buffer[4] = SP_VERSION_MAJOR;
    buffer[5] = SP_VERSION_MINOR;
    buffer[6] = type;
    buffer[7] = 0;
    put_u16(buffer + 8, 0);
    put_u32(buffer + 10, length);
    if (length)
        memcpy(buffer + SP_HEADER_SIZE, payload, length);

    output->next = 0;
    output->buffer = buffer;
    output->length = SP_HEADER_SIZE + length;
    output->offset = 0;
    output->data_length = data_length;
    *h->output_tail = output;
    h->output_tail = &output->next;
    h->output_length += data_length;
    return 1;
}

static int
push_output(session_handle *h)
{
    while (h->output_head) {
        session_output *output = h->output_head;
        ssize_t count = write(h->fd, output->buffer + output->offset,
                              output->length - output->offset);

        if (count > 0) {
            output->offset += (size_t) count;
            if (output->offset == output->length) {
                h->output_head = output->next;
                if (!h->output_head)
                    h->output_tail = &h->output_head;
                h->output_length -= output->data_length;
                free(output->buffer);
                free(output);
            }
        } else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 1;
        else
            return 0;
    }
    return 1;
}

static int
drain_output(session_handle *h)
{
    struct pollfd writable = {h->fd, POLLOUT, 0};
    int attempts = 4;

    while (h->output_head) {
        int result;

        if (!push_output(h))
            return 0;
        if (!h->output_head)
            return 1;
        do {
            result = poll(&writable, 1, 250);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || !(writable.revents & POLLOUT) || --attempts == 0)
            return 0;
    }
    return 1;
}

static int
finish_detach(session_handle *h)
{
    struct pollfd readable = {h->fd, POLLIN, 0};
    unsigned char discard[256];
    int attempts = 4;

    if (shutdown(h->fd, SHUT_WR) < 0)
        return 0;
    while (attempts-- > 0) {
        int result;
        do {
            result = poll(&readable, 1, 250);
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return 0;
        if (result == 0)
            continue;
        {
            ssize_t count = read(h->fd, discard, sizeof(discard));
            if (count == 0)
                return 1;
            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                return 0;
        }
    }
    return 0;
}

static void
free_output(session_handle *h)
{
    while (h->output_head) {
        session_output *output = h->output_head;
        h->output_head = output->next;
        free(output->buffer);
        free(output);
    }
}

static void
remove_handle(session_handle *h, int notify_server)
{
    if (notify_server && h->attached)
        server_close(h->shandle);
    *h->prev = h->next;
    if (h->next)
        h->next->prev = h->prev;
    close(h->fd);
    free_output(h);
    free(h->payload);
    free_stream(h->input);
    free(h->name);
    free(h);
}

static int
queue_ack(session_handle *h, uint8_t direction, uint64_t position)
{
    struct sp_ack ack = {direction, position};
    unsigned char payload[SP_ACK_SIZE];

    sp_encode_ack(&ack, payload);
    return queue_frame(h, SP_ACK, payload, sizeof(payload), 0);
}

static int
queue_welcome(session_handle *h, const struct sp_hello *hello)
{
    struct sp_welcome welcome = {
        hello->generation, hello->mode,
        hello->input_position, hello->output_position
    };
    unsigned char payload[SP_WELCOME_SIZE];

    sp_encode_welcome(&welcome, payload);
    return queue_frame(h, SP_WELCOME, payload, sizeof(payload), 0);
}

static int
consume_input(session_handle *h, const unsigned char *buffer, size_t count)
{
    Stream *s = h->input;
    const unsigned char *ptr, *end;

    if (h->excess_utf_count) {
        int needed = clearance_utf(h->excess_utf[0]);
        int available = needed - h->excess_utf_count;
        int take = count < (size_t) available ? (int) count : available;

        memcpy(h->excess_utf + h->excess_utf_count, buffer, (size_t) take);
        h->excess_utf_count += take;
        buffer += take;
        count -= (size_t) take;
        if (h->excess_utf_count == needed) {
            const char *p = (const char *) h->excess_utf;
            int c = get_utf(&p);
            if (my_is_printable(c))
                stream_add_utf(s, c);
            h->excess_utf_count = 0;
        }
    }

    if (h->binary) {
        stream_add_moobinary_from_raw_bytes(s, (const char *) buffer, count);
        server_receive_line(h->shandle, reset_stream(s));
        h->last_input_was_CR = 0;
        h->excess_utf_count = 0;
        return 1;
    }

    for (ptr = buffer, end = buffer + count;
         ptr < end && ptr + clearance_utf(*ptr) <= end;) {
        int c = get_utf((const char **) &ptr);

        if (my_is_printable(c))
            stream_add_utf(s, c);
#ifdef INPUT_APPLY_BACKSPACE
        else if (c == 0x08 || c == 0x7f)
            stream_delete_utf(s);
#endif
        else if (c == '\r' || (c == '\n' && !h->last_input_was_CR))
            server_receive_line(h->shandle, reset_stream(s));

        h->last_input_was_CR = c == '\r';
    }
    if (ptr < end) {
        h->excess_utf_count = (int) (end - ptr);
        memcpy(h->excess_utf, ptr, (size_t) h->excess_utf_count);
    }
    return 1;
}

static int
handle_hello(session_handle *h, struct sp_frame *frame)
{
    struct sp_hello hello;
    network_handle nh;
    server_listener sl;

    if (h->attached || sp_decode_hello(frame, &hello) < 0)
        return 0;
    if (hello.player != INT64_MIN
        && hello.mode == SP_MODE_INITIAL)
        return 0;

    h->input_position = hello.input_position;
    h->output_position = hello.output_position;
    h->output_acked = hello.output_position;
    free(h->name);
    h->name = malloc((size_t) hello.origin_length + 1);
    if (!h->name)
        return 0;
    memcpy(h->name, hello.origin, hello.origin_length);
    h->name[hello.origin_length] = '\0';

    if (!queue_welcome(h, &hello))
        return 0;

    nh.ptr = h;
    sl = the_listener->slistener;
    if (hello.player == INT64_MIN)
        h->shandle = server_new_connection(sl, nh, 0);
    else
        h->shandle = server_resume_connection(
            sl, nh, (Objid) hello.player, (Objid) hello.listener,
            hello.mode == SP_MODE_CRASH);
    if (!h->shandle.ptr)
        return 0;
    h->attached = 1;

    if (hello.player != INT64_MIN && hello.mode == SP_MODE_CRASH)
        (void) network_send_line(nh,
            "*** Session recovered from a server crash ***", 1);
    return 1;
}

static int
handle_frame(session_handle *h)
{
    struct sp_frame frame = {
        h->frame_type, 0, h->payload_length, h->payload
    };

    if (!h->attached)
        return h->frame_type == SP_HELLO && handle_hello(h, &frame);

    switch (h->frame_type) {
    case SP_INPUT: {
        uint64_t position = sp_get_u64(h->payload);
        size_t length = h->payload_length - 8;

        if (position != h->input_position)
            return 0;
        if (!consume_input(h, h->payload + 8, length))
            return 0;
        h->input_position += length;
        return queue_ack(h, SP_DIR_INPUT, h->input_position);
    }
    case SP_ACK: {
        struct sp_ack ack;
        if (sp_decode_ack(&frame, &ack) < 0
            || ack.direction != SP_DIR_OUTPUT
            || ack.position < h->output_acked
            || ack.position > h->output_position)
            return 0;
        h->output_acked = ack.position;
        return 1;
    }
    case SP_CLOSE:
        return -1;
    default:
        return 0;
    }
}

static int
parse_header(session_handle *h)
{
    if (get_u32(h->header) != SP_MAGIC
        || h->header[4] != SP_VERSION_MAJOR
        || h->header[5] > SP_VERSION_MINOR
        || h->header[7] != 0 || get_u16(h->header + 8) != 0)
        return 0;
    h->frame_type = h->header[6];
    h->payload_length = get_u32(h->header + 10);
    if (!valid_type_and_length(h->frame_type, h->payload_length))
        return 0;
    h->payload = h->payload_length ? malloc(h->payload_length) : NULL;
    if (h->payload_length && !h->payload)
        return 0;
    h->payload_used = 0;
    return 1;
}

static int
pull_input(session_handle *h)
{
    for (;;) {
        unsigned char *target;
        size_t wanted;
        ssize_t count;

        if (h->header_used < SP_HEADER_SIZE) {
            target = h->header + h->header_used;
            wanted = SP_HEADER_SIZE - h->header_used;
        } else {
            target = h->payload + h->payload_used;
            wanted = h->payload_length - h->payload_used;
        }

        if (wanted == 0) {
            int result = handle_frame(h);
            free(h->payload);
            h->payload = NULL;
            h->payload_length = 0;
            h->payload_used = 0;
            h->header_used = 0;
            if (result <= 0)
                return result;
            continue;
        }

        count = read(h->fd, target, wanted);
        if (count > 0) {
            if (h->header_used < SP_HEADER_SIZE) {
                h->header_used += (size_t) count;
                if (h->header_used == SP_HEADER_SIZE && !parse_header(h))
                    return 0;
            } else
                h->payload_used += (size_t) count;
        } else if (count == 0)
            return 0;
        else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 1;
        else
            return 0;
    }
}

static session_handle *
new_handle(int fd)
{
    session_handle *h = calloc(1, sizeof(*h));

    if (!h)
        return NULL;
    h->fd = fd;
    h->input = new_stream(100);
    h->name = strdup("unattached session");
    h->output_tail = &h->output_head;
    h->next = all_handles;
    h->prev = &all_handles;
    if (all_handles)
        all_handles->prev = &h->next;
    all_handles = h;
    return h;
}

static void
accept_session(void)
{
    int fd = accept(the_listener->fd, NULL, NULL);

    if (fd < 0)
        return;
    if (!peer_is_allowed(fd) || !set_nonblocking(fd) || !new_handle(fd))
        close(fd);
}

const char *
network_protocol_name(void)
{
    return "connection-independent sessions";
}

const char *
network_usage_string(void)
{
    return "[session-unix-socket]";
}

int
network_initialize(int argc, char **argv, Var *desc)
{
    if (argc > 1)
        return 0;
    desc->type = TYPE_STR;
    desc->v.str = str_dup(argc == 1 ? argv[0] : DEFAULT_CONNECT_FILE);
    signal(SIGPIPE, SIG_IGN);
    return 1;
}

enum error
network_make_listener(server_listener sl, Var desc, network_listener *nl,
                      Var *canon, const char **name)
{
    struct sockaddr_un address;
    session_listener *listener;
    int fd;

    if (the_listener || desc.type != TYPE_STR
        || strlen(desc.v.str) >= sizeof(address.sun_path))
        return E_INVARG;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return E_QUOTA;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, desc.v.str, strlen(desc.v.str) + 1);
    unlink(desc.v.str);
    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) < 0
        || chmod(desc.v.str, 0600) < 0 || !set_nonblocking(fd)) {
        close(fd);
        return E_QUOTA;
    }

    listener = calloc(1, sizeof(*listener));
    if (!listener) {
        close(fd);
        return E_QUOTA;
    }
    listener->fd = fd;
    listener->slistener = sl;
    listener->path = strdup(desc.v.str);
    listener->name = listener->path;
    the_listener = listener;
    nl->ptr = listener;
    canon->type = TYPE_STR;
    canon->v.str = str_dup(desc.v.str);
    *name = listener->name;
    return E_NONE;
}

int
network_listen(network_listener nl)
{
    session_listener *listener = nl.ptr;
    return listen(listener->fd, 128) == 0;
}

static int
queue_data(session_handle *h, const unsigned char *data, size_t length,
           int flush_ok)
{
    while (length) {
        size_t chunk = length > SP_MAX_DATA ? SP_MAX_DATA : length;
        unsigned char *payload;

        if (h->output_length + chunk > MAX_QUEUED_OUTPUT) {
            if (!push_output(h)
                || (h->output_length + chunk > MAX_QUEUED_OUTPUT
                    && !flush_ok))
                return 0;
        }
        payload = malloc(chunk + 8);
        if (!payload)
            return 0;
        sp_put_u64(payload, h->output_position);
        memcpy(payload + 8, data, chunk);
        if (!queue_frame(h, SP_OUTPUT, payload, (uint32_t) chunk + 8, chunk)) {
            free(payload);
            return 0;
        }
        free(payload);
        h->output_position += chunk;
        data += chunk;
        length -= chunk;
    }
    return 1;
}

int
network_send_line(network_handle nh, const char *line, int flush_ok)
{
    session_handle *h = nh.ptr;
    size_t length = strlen(line);
    unsigned char *buffer = malloc(length + 2);
    int result;

    if (!buffer)
        return 0;
    memcpy(buffer, line, length);
    buffer[length] = '\r';
    buffer[length + 1] = '\n';
    result = queue_data(h, buffer, length + 2, flush_ok);
    free(buffer);
    return result;
}

int
network_send_bytes(network_handle nh, const char *buffer, size_t buflen,
                   int flush_ok)
{
    return queue_data(nh.ptr, (const unsigned char *) buffer, buflen, flush_ok);
}

int
network_buffered_output_length(network_handle nh)
{
    session_handle *h = nh.ptr;
    return h->output_length > INT_MAX ? INT_MAX : (int) h->output_length;
}

void
network_suspend_input(network_handle nh)
{
    ((session_handle *) nh.ptr)->input_suspended = 1;
}

void
network_resume_input(network_handle nh)
{
    ((session_handle *) nh.ptr)->input_suspended = 0;
}

void
network_set_connection_binary(network_handle nh, int binary)
{
    ((session_handle *) nh.ptr)->binary = binary;
}

void
network_set_connection_player(network_handle nh, Objid player, Objid listener)
{
    session_handle *h = nh.ptr;
    struct sp_bind bind = {(int64_t) player, (int64_t) listener};
    unsigned char payload[SP_BIND_SIZE];

    sp_encode_bind(&bind, payload);
    (void) queue_frame(h, SP_BIND, payload, sizeof(payload), 0);
}

#define NETWORK_CO_TABLE(DEFINE, nh, value, _)
    /* No carrier-specific connection options. */

int
network_process_io(int timeout)
{
    session_handle *h, *next;

    mplex_clear();
    if (the_listener)
        mplex_add_reader(the_listener->fd);
    for (h = all_handles; h; h = h->next) {
        if (!h->input_suspended)
            mplex_add_reader(h->fd);
        if (h->output_head)
            mplex_add_writer(h->fd);
    }

    if (mplex_wait(timeout))
        return 0;
    if (the_listener && mplex_is_readable(the_listener->fd))
        accept_session();

    for (h = all_handles; h; h = next) {
        int okay = 1;
        next = h->next;
        if (mplex_is_readable(h->fd)) {
            int result = pull_input(h);
            if (result <= 0) {
                remove_handle(h, result == 0);
                continue;
            }
        }
        if (mplex_is_writable(h->fd))
            okay = push_output(h);
        if (!okay)
            remove_handle(h, 1);
    }
    return 1;
}

const char *
network_connection_name(network_handle nh)
{
    return ((session_handle *) nh.ptr)->name;
}

void
network_close(network_handle nh)
{
    session_handle *h = nh.ptr;
    (void) queue_frame(h, SP_CLOSE, NULL, 0, 0);
    (void) push_output(h);
    remove_handle(h, 0);
}

void
network_close_listener(network_listener nl)
{
    session_listener *listener = nl.ptr;

    if (!listener)
        return;
    close(listener->fd);
    unlink(listener->path);
    free(listener->path);
    free(listener);
    the_listener = NULL;
}

void
network_shutdown(void)
{
    session_handle *h;

    for (h = all_handles; h; h = h->next) {
        struct sp_detach detach = {
            SP_MODE_GRACEFUL, h->input_position, h->output_position
        };
        unsigned char payload[SP_DETACH_SIZE];

        sp_encode_detach(&detach, payload);
        if (!queue_frame(h, SP_DETACH, payload, sizeof(payload), 0)
            || !drain_output(h) || !finish_detach(h))
            errlog("SESSION DETACH: graceful completion failed\n");
    }
    while (all_handles)
        remove_handle(all_handles, 0);
    if (the_listener) {
        network_listener nl;
        nl.ptr = the_listener;
        network_close_listener(nl);
    }
}
