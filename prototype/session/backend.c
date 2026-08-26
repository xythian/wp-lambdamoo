#define _GNU_SOURCE
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct worker {
    int fd;
    char instance[64];
};

static int
write_ready(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (write(fd, "ready\n", 6) != 6) {
        close(fd);
        return -1;
    }
    return close(fd);
}

static int
send_output(int fd, uint64_t *position, const void *data, size_t length)
{
    const unsigned char *source = data;

    while (length != 0) {
        size_t chunk = length > SP_MAX_DATA ? SP_MAX_DATA : length;
        unsigned char *payload = malloc(chunk + 8);
        int result;

        if (!payload)
            return -1;
        sp_put_u64(payload, *position);
        memcpy(payload + 8, source, chunk);
        result = sp_frame_write(fd, SP_OUTPUT, 0, payload,
                                (uint32_t) chunk + 8);
        free(payload);
        if (result < 0)
            return -1;
        *position += chunk;
        source += chunk;
        length -= chunk;
    }
    return 0;
}

static int
send_ack(int fd, uint8_t direction, uint64_t position)
{
    struct sp_ack ack = {direction, position};
    unsigned char payload[SP_ACK_SIZE];

    sp_encode_ack(&ack, payload);
    return sp_frame_write(fd, SP_ACK, 0, payload, sizeof(payload));
}

static int
is_exact(const unsigned char *data, size_t length, const char *text)
{
    return length == strlen(text) && memcmp(data, text, length) == 0;
}

static int
bulk_size(const unsigned char *data, size_t length, size_t *size)
{
    char buffer[64];
    char extra;
    unsigned long parsed;

    if (length >= sizeof(buffer))
        return 0;
    memcpy(buffer, data, length);
    buffer[length] = '\0';
    if (sscanf(buffer, "BULK %lu%c", &parsed, &extra) != 2 || extra != '\n'
        || parsed > 16u * 1024u * 1024u)
        return 0;
    *size = (size_t) parsed;
    return 1;
}

static int
send_attach_banner(int fd, uint64_t *position, uint8_t mode,
                   const char *instance)
{
    char banner[160];
    const char *name = mode == SP_MODE_INITIAL ? "initial"
        : mode == SP_MODE_GRACEFUL ? "graceful" : "crash";
    int length = snprintf(banner, sizeof(banner), "ATTACHED %s %s\n",
                          name, instance);

    return length > 0 && (size_t) length < sizeof(banner)
        ? send_output(fd, position, banner, (size_t) length) : -1;
}

static void
exit_after_graceful(void)
{
    _exit(0);
}

static void *
serve_session(void *argument)
{
    struct worker *worker = argument;
    struct sp_frame frame;
    struct sp_hello hello;
    struct sp_welcome welcome;
    unsigned char welcome_payload[SP_WELCOME_SIZE];
    uint64_t input_position;
    uint64_t output_position;
    int fd = worker->fd;

    if (sp_frame_read(fd, &frame) != 1 || sp_decode_hello(&frame, &hello) < 0)
        goto done;
    input_position = hello.input_position;
    output_position = hello.output_position;
    welcome = (struct sp_welcome) {
        hello.generation, hello.mode, input_position, output_position
    };
    sp_encode_welcome(&welcome, welcome_payload);
    if (sp_frame_write(fd, SP_WELCOME, 0, welcome_payload,
                       sizeof(welcome_payload)) < 0
        || send_attach_banner(fd, &output_position, hello.mode,
                              worker->instance) < 0) {
        sp_frame_clear(&frame);
        goto done;
    }
    sp_frame_clear(&frame);

    for (;;) {
        struct sp_ack ack;
        uint64_t offset;
        const unsigned char *data;
        size_t length;
        int result = sp_frame_read(fd, &frame);

        if (result != 1)
            break;
        if (frame.type == SP_INPUT) {
            size_t requested;
            offset = sp_get_u64(frame.payload);
            data = frame.payload + 8;
            length = frame.length - 8;
            if (offset != input_position) {
                sp_frame_clear(&frame);
                break;
            }
            input_position += length;
            if (send_ack(fd, SP_DIR_INPUT, input_position) < 0) {
                sp_frame_clear(&frame);
                break;
            }
            if (is_exact(data, length, "GRACEFUL\n")) {
                struct sp_detach detach = {
                    SP_MODE_GRACEFUL, input_position, output_position
                };
                unsigned char payload[SP_DETACH_SIZE];
                sp_encode_detach(&detach, payload);
                (void) sp_frame_write(fd, SP_DETACH, 0, payload,
                                      sizeof(payload));
                sp_frame_clear(&frame);
                shutdown(fd, SHUT_WR);
                exit_after_graceful();
                break;
            }
            if (bulk_size(data, length, &requested)) {
                unsigned char chunk[65536];
                size_t remaining = requested;
                memset(chunk, 'X', sizeof(chunk));
                while (remaining != 0) {
                    size_t count = remaining > sizeof(chunk)
                        ? sizeof(chunk) : remaining;
                    if (send_output(fd, &output_position, chunk, count) < 0)
                        break;
                    remaining -= count;
                }
                if (remaining != 0) {
                    sp_frame_clear(&frame);
                    break;
                }
            } else if (send_output(fd, &output_position, data, length) < 0) {
                sp_frame_clear(&frame);
                break;
            }
        } else if (frame.type == SP_ACK) {
            if (sp_decode_ack(&frame, &ack) < 0
                || ack.direction != SP_DIR_OUTPUT
                || ack.position > output_position) {
                sp_frame_clear(&frame);
                break;
            }
        } else if (frame.type == SP_CLOSE) {
            sp_frame_clear(&frame);
            break;
        } else {
            sp_frame_clear(&frame);
            break;
        }
        sp_frame_clear(&frame);
    }

done:
    close(fd);
    free(worker);
    return NULL;
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

int
main(int argc, char **argv)
{
    struct sockaddr_un address;
    int listener;

    if (argc != 4) {
        fprintf(stderr, "usage: %s SOCKET INSTANCE READY_FILE\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
        goto failure;
    if (strlen(argv[1]) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        goto failure;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, argv[1], strlen(argv[1]) + 1);
    unlink(argv[1]);
    if (bind(listener, (struct sockaddr *) &address, sizeof(address)) < 0
        || chmod(argv[1], 0600) < 0 || listen(listener, 128) < 0
        || write_ready(argv[3]) < 0)
        goto failure;

    for (;;) {
        struct worker *worker;
        pthread_t thread;
        int fd = accept(listener, NULL, NULL);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            goto failure;
        }
        if (!peer_is_allowed(fd)) {
            close(fd);
            continue;
        }
        worker = calloc(1, sizeof(*worker));
        if (!worker) {
            close(fd);
            continue;
        }
        worker->fd = fd;
        snprintf(worker->instance, sizeof(worker->instance), "%s", argv[2]);
        if (pthread_create(&thread, NULL, serve_session, worker) != 0) {
            close(fd);
            free(worker);
            continue;
        }
        pthread_detach(thread);
    }

failure:
    perror("backend");
    return 1;
}
