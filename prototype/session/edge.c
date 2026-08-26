#define _GNU_SOURCE
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct edge_config {
    SSL_CTX *tls;
    char backend_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    unsigned char edge_id[SP_ID_SIZE];
};

struct client {
    SSL *tls;
    int fd;
    struct edge_config *config;
    unsigned char session_id[SP_ID_SIZE];
    char origin[128];
    uint64_t generation;
    uint64_t input_position;
    uint64_t input_acked;
    uint64_t output_position;
    uint8_t next_mode;
    int64_t player;
    int64_t listener;
};

static void
delay_reconnect(void)
{
    struct timespec delay = {0, 50000000};
    nanosleep(&delay, NULL);
}

static int
write_ready(const char *path, unsigned port)
{
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "%u\n", port);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return -1;
    if (length <= 0 || write(fd, buffer, (size_t) length) != length) {
        close(fd);
        return -1;
    }
    return close(fd);
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
connect_backend(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        return -1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) < 0
        || !peer_is_allowed(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
tls_write_all(SSL *tls, const unsigned char *data, size_t length)
{
    size_t done = 0;

    while (done < length) {
        size_t written = 0;
        int result = SSL_write_ex(tls, data + done, length - done, &written);
        if (result == 1) {
            done += written;
            continue;
        }
        result = SSL_get_error(tls, result);
        if (result != SSL_ERROR_WANT_READ && result != SSL_ERROR_WANT_WRITE)
            return -1;
    }
    return 0;
}

static int
send_hello(struct client *client, int backend)
{
    struct sp_hello hello;
    struct sp_frame frame;
    struct sp_welcome welcome;
    unsigned char *payload;
    uint32_t length;
    int result;

    memset(&hello, 0, sizeof(hello));
    memcpy(hello.session_id, client->session_id, SP_ID_SIZE);
    memcpy(hello.edge_id, client->config->edge_id, SP_ID_SIZE);
    hello.generation = client->generation;
    hello.mode = client->next_mode;
    hello.input_position = client->input_position;
    hello.output_position = client->output_position;
    hello.player = client->player;
    hello.listener = client->listener;
    hello.origin = (const unsigned char *) client->origin;
    hello.origin_length = (uint16_t) strlen(client->origin);
    if (sp_encode_hello(&hello, &payload, &length) < 0)
        return -1;
    result = sp_frame_write(backend, SP_HELLO, 0, payload, length);
    free(payload);
    if (result < 0 || sp_frame_read(backend, &frame) != 1)
        return -1;
    result = sp_decode_welcome(&frame, &welcome);
    if (result == 0
        && (welcome.generation != client->generation
            || welcome.mode != client->next_mode
            || welcome.input_position != client->input_position
            || welcome.output_position != client->output_position))
        result = -1;
    sp_frame_clear(&frame);
    return result;
}

static int
attach_backend(struct client *client)
{
    for (;;) {
        int fd = connect_backend(client->config->backend_path);
        if (fd >= 0) {
            client->generation++;
            if (send_hello(client, fd) == 0)
                return fd;
            close(fd);
        }
        delay_reconnect();
    }
}

static int
send_input(int backend, struct client *client,
           const unsigned char *data, size_t length)
{
    unsigned char *payload = malloc(length + 8);
    int result;

    if (!payload)
        return -1;
    sp_put_u64(payload, client->input_position);
    memcpy(payload + 8, data, length);
    result = sp_frame_write(backend, SP_INPUT, 0, payload,
                            (uint32_t) length + 8);
    free(payload);
    if (result == 1)
        client->input_position += length;
    return result;
}

static int
send_output_ack(int backend, uint64_t position)
{
    struct sp_ack ack = {SP_DIR_OUTPUT, position};
    unsigned char payload[SP_ACK_SIZE];

    sp_encode_ack(&ack, payload);
    return sp_frame_write(backend, SP_ACK, 0, payload, sizeof(payload));
}

static int
handle_backend(struct client *client, int backend)
{
    struct sp_frame frame;
    int result = sp_frame_read(backend, &frame);

    if (result != 1)
        return -1;
    if (frame.type == SP_OUTPUT) {
        uint64_t position = sp_get_u64(frame.payload);
        size_t length = frame.length - 8;
        if (position != client->output_position
            || tls_write_all(client->tls, frame.payload + 8, length) < 0) {
            sp_frame_clear(&frame);
            return -2;
        }
        client->output_position += length;
        result = send_output_ack(backend, client->output_position);
    } else if (frame.type == SP_ACK) {
        struct sp_ack ack;
        result = sp_decode_ack(&frame, &ack);
        if (result == 0) {
            if (ack.direction != SP_DIR_INPUT
                || ack.position < client->input_acked
                || ack.position > client->input_position)
                result = -1;
            else
                client->input_acked = ack.position;
            if (result == 0)
                result = 1;
        }
    } else if (frame.type == SP_BIND) {
        struct sp_bind bind;
        result = sp_decode_bind(&frame, &bind);
        if (result == 0) {
            client->player = bind.player;
            client->listener = bind.listener;
            result = 1;
        }
    } else if (frame.type == SP_DETACH) {
        struct sp_detach detach;
        result = sp_decode_detach(&frame, &detach);
        if (result == 0 && detach.mode == SP_MODE_GRACEFUL
            && detach.input_position == client->input_position
            && detach.output_position == client->output_position) {
            client->next_mode = SP_MODE_GRACEFUL;
            result = 2;
        } else {
            fprintf(stderr,
                    "edge: invalid detach mode=%u input=%llu/%llu output=%llu/%llu\n",
                    (unsigned) detach.mode,
                    (unsigned long long) detach.input_position,
                    (unsigned long long) client->input_position,
                    (unsigned long long) detach.output_position,
                    (unsigned long long) client->output_position);
            result = -1;
        }
    } else if (frame.type == SP_CLOSE)
        result = 0;
    else
        result = -1;
    sp_frame_clear(&frame);
    return result;
}

static void *
serve_client(void *argument)
{
    struct client *client = argument;
    unsigned char input[65536];
    int backend = attach_backend(client);

    for (;;) {
        struct pollfd fds[2] = {
            {client->fd, POLLIN, 0}, {backend, POLLIN, 0}
        };
        int polled = poll(fds, 2, -1);

        if (polled < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            int result = handle_backend(client, backend);
            if (result <= 0) {
                if (result == 0)
                    break;
                close(backend);
                client->next_mode = SP_MODE_CRASH;
                backend = attach_backend(client);
                continue;
            }
            if (result == 2) {
                close(backend);
                backend = attach_backend(client);
                client->next_mode = SP_MODE_CRASH;
                continue;
            }
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            size_t count = 0;
            int result = SSL_read_ex(client->tls, input, sizeof(input), &count);
            if (result != 1 || count == 0) {
                (void) sp_frame_write(backend, SP_CLOSE, 0, NULL, 0);
                break;
            }
            if (send_input(backend, client, input, count) < 0) {
                close(backend);
                client->next_mode = SP_MODE_CRASH;
                backend = attach_backend(client);
            }
        }
    }

    close(backend);
    SSL_shutdown(client->tls);
    SSL_free(client->tls);
    close(client->fd);
    free(client);
    return NULL;
}

static int
make_listener(const char *bind_address, unsigned port, unsigned *actual_port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int yes = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1
        || bind(fd, (struct sockaddr *) &address, sizeof(address)) < 0
        || listen(fd, 128) < 0
        || getsockname(fd, (struct sockaddr *) &address, &length) < 0) {
        close(fd);
        return -1;
    }
    *actual_port = ntohs(address.sin_port);
    return fd;
}

static SSL_CTX *
make_tls_context(const char *certificate, const char *key)
{
    SSL_CTX *context = SSL_CTX_new(TLS_server_method());

    if (!context)
        return NULL;
    if (!SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION)
        || SSL_CTX_use_certificate_chain_file(context, certificate) != 1
        || SSL_CTX_use_PrivateKey_file(context, key, SSL_FILETYPE_PEM) != 1
        || SSL_CTX_check_private_key(context) != 1) {
        SSL_CTX_free(context);
        return NULL;
    }
    return context;
}

int
main(int argc, char **argv)
{
    struct edge_config config;
    unsigned requested_port, actual_port;
    int listener;

    if (argc != 7) {
        fprintf(stderr,
                "usage: %s CERT KEY BACKEND_SOCKET BIND_ADDRESS PORT READY_FILE\n",
                argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    memset(&config, 0, sizeof(config));
    config.tls = make_tls_context(argv[1], argv[2]);
    if (!config.tls || RAND_bytes(config.edge_id, sizeof(config.edge_id)) != 1) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    if (strlen(argv[3]) >= sizeof(config.backend_path)
        || sscanf(argv[5], "%u", &requested_port) != 1
        || requested_port > 65535) {
        fprintf(stderr, "invalid argument\n");
        return 2;
    }
    memcpy(config.backend_path, argv[3], strlen(argv[3]) + 1);
    listener = make_listener(argv[4], requested_port, &actual_port);
    if (listener < 0 || write_ready(argv[6], actual_port) < 0) {
        perror("edge");
        return 1;
    }

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        struct client *client;
        pthread_t thread;
        int fd = accept(listener, (struct sockaddr *) &peer, &peer_length);

        if (fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            return 1;
        }
        client = calloc(1, sizeof(*client));
        if (!client) {
            close(fd);
            continue;
        }
        client->tls = SSL_new(config.tls);
        client->fd = fd;
        client->config = &config;
        client->next_mode = SP_MODE_INITIAL;
        client->player = INT64_MIN;
        if (!client->tls
            || RAND_bytes(client->session_id, sizeof(client->session_id)) != 1) {
            SSL_free(client->tls);
            close(fd);
            free(client);
            continue;
        }
        SSL_set_fd(client->tls, fd);
        if (SSL_accept(client->tls) != 1) {
            SSL_free(client->tls);
            close(fd);
            free(client);
            continue;
        }
        snprintf(client->origin, sizeof(client->origin), "tls:%s:%u",
                 inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        if (pthread_create(&thread, NULL, serve_client, client) != 0) {
            SSL_free(client->tls);
            close(fd);
            free(client);
            continue;
        }
        pthread_detach(thread);
    }
}
