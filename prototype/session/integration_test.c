#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct tls_client {
    SSL *ssl;
    int fd;
};

static pid_t children[8];
static size_t child_count;

static void
cleanup(void)
{
    size_t i;
    for (i = 0; i < child_count; i++)
        if (children[i] > 0)
            kill(children[i], SIGKILL);
    for (i = 0; i < child_count; i++)
        if (children[i] > 0)
            (void) waitpid(children[i], NULL, 0);
}

static void
fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "integration failure: ");
    vfprintf(stderr, format, arguments);
    fprintf(stderr, "\n");
    va_end(arguments);
    cleanup();
    exit(1);
}

static pid_t
spawn(char *const arguments[])
{
    pid_t child = fork();
    if (child < 0)
        fail("fork: %s", strerror(errno));
    if (child == 0) {
        execv(arguments[0], arguments);
        perror("execv");
        _exit(127);
    }
    if (child_count == sizeof(children) / sizeof(children[0]))
        fail("too many children");
    children[child_count++] = child;
    return child;
}

static void
forget_child(pid_t child)
{
    size_t i;
    for (i = 0; i < child_count; i++)
        if (children[i] == child)
            children[i] = 0;
}

static void
stop_child(pid_t child, int signal_number)
{
    int status;
    if (kill(child, signal_number) < 0 && errno != ESRCH)
        fail("kill: %s", strerror(errno));
    if (waitpid(child, &status, 0) < 0)
        fail("waitpid: %s", strerror(errno));
    forget_child(child);
}

static void
short_delay(void)
{
    struct timespec delay = {0, 50000000};
    nanosleep(&delay, NULL);
}

static void
wait_for_file(const char *path)
{
    unsigned attempts;
    struct stat status;

    for (attempts = 0; attempts < 200; attempts++) {
        if (stat(path, &status) == 0)
            return;
        short_delay();
    }
    fail("timed out waiting for %s", path);
}

static unsigned
read_port(const char *path)
{
    FILE *file = fopen(path, "r");
    unsigned port;
    if (!file || fscanf(file, "%u", &port) != 1 || fclose(file) != 0)
        fail("cannot read port from %s", path);
    return port;
}

static pid_t
start_backend(const char *binary, const char *socket_path,
              const char *instance, const char *ready)
{
    char *arguments[] = {
        (char *) binary, (char *) socket_path, (char *) instance,
        (char *) ready, NULL
    };
    unlink(ready);
    pid_t child = spawn(arguments);
    wait_for_file(ready);
    return child;
}

static pid_t
start_edge(const char *binary, const char *certificate, const char *key,
           const char *socket_path, const char *ready)
{
    char *arguments[] = {
        (char *) binary, (char *) certificate, (char *) key,
        (char *) socket_path, "127.0.0.1", "0", (char *) ready, NULL
    };
    unlink(ready);
    pid_t child = spawn(arguments);
    wait_for_file(ready);
    return child;
}

static struct tls_client
connect_client(SSL_CTX *context, unsigned port)
{
    struct sockaddr_in address;
    struct timeval timeout = {5, 0};
    struct tls_client client;

    client.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client.fd < 0)
        fail("client socket: %s", strerror(errno));
    setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(client.fd, (struct sockaddr *) &address, sizeof(address)) < 0)
        fail("client connect: %s", strerror(errno));

    client.ssl = SSL_new(context);
    if (!client.ssl || SSL_set1_host(client.ssl, "localhost") != 1
        || SSL_set_fd(client.ssl, client.fd) != 1
        || SSL_connect(client.ssl) != 1
        || SSL_get_verify_result(client.ssl) != X509_V_OK) {
        ERR_print_errors_fp(stderr);
        fail("verified TLS connection failed");
    }
    return client;
}

static void
close_client(struct tls_client *client)
{
    SSL_shutdown(client->ssl);
    SSL_free(client->ssl);
    close(client->fd);
    client->ssl = NULL;
    client->fd = -1;
}

static void
send_bytes(struct tls_client *client, const void *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        size_t count = 0;
        if (SSL_write_ex(client->ssl, (const unsigned char *) data + done,
                         length - done, &count) != 1)
            fail("TLS write failed");
        done += count;
    }
}

static void
expect_bytes(struct tls_client *client, const void *expected, size_t length)
{
    const unsigned char *wanted = expected;
    unsigned char buffer[65536];
    size_t done = 0;

    while (done < length) {
        size_t count = 0;
        size_t request = length - done > sizeof(buffer)
            ? sizeof(buffer) : length - done;
        if (SSL_read_ex(client->ssl, buffer, request, &count) != 1 || count == 0)
            fail("TLS read failed after %zu of %zu bytes", done, length);
        if (memcmp(buffer, wanted + done, count) != 0)
            fail("TLS data mismatch at byte %zu", done);
        done += count;
    }
}

static void
expect_fill(struct tls_client *client, unsigned char value, size_t length)
{
    unsigned char buffer[65536];
    size_t done = 0;

    while (done < length) {
        size_t count = 0;
        size_t request = length - done > sizeof(buffer)
            ? sizeof(buffer) : length - done;
        if (SSL_read_ex(client->ssl, buffer, request, &count) != 1 || count == 0)
            fail("bulk TLS read failed after %zu of %zu bytes", done, length);
        for (size_t i = 0; i < count; i++)
            if (buffer[i] != value)
                fail("bulk data mismatch at byte %zu", done + i);
        done += count;
    }
}

static void
expect_text(struct tls_client *client, const char *text)
{
    expect_bytes(client, text, strlen(text));
}

static void
send_malformed(const char *path)
{
    struct sockaddr_un address;
    unsigned char bad_header[14] = {0};
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        fail("malformed socket");
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) < 0
        || write(fd, bad_header, sizeof(bad_header))
            != (ssize_t) sizeof(bad_header))
        fail("malformed injection");
    close(fd);
}

int
main(int argc, char **argv)
{
    const char *edge_binary, *backend_binary, *certificate, *key;
    const char *directory;
    char socket_path[256], backend_ready[256], edge1_ready[256], edge2_ready[256];
    SSL_CTX *context;
    pid_t backend, edge1, edge2;
    struct tls_client primary, slow, fast, second_edge;
    unsigned port1, port2;
    const size_t bulk = 3u * 1024u * 1024u;
    char command[64];

    if (argc != 6) {
        fprintf(stderr, "usage: %s EDGE BACKEND CERT KEY TMPDIR\n", argv[0]);
        return 2;
    }
    edge_binary = argv[1];
    backend_binary = argv[2];
    certificate = argv[3];
    key = argv[4];
    directory = argv[5];
    snprintf(socket_path, sizeof(socket_path), "%s/backend.sock", directory);
    snprintf(backend_ready, sizeof(backend_ready), "%s/backend.ready", directory);
    snprintf(edge1_ready, sizeof(edge1_ready), "%s/edge1.ready", directory);
    snprintf(edge2_ready, sizeof(edge2_ready), "%s/edge2.ready", directory);

    signal(SIGPIPE, SIG_IGN);
    context = SSL_CTX_new(TLS_client_method());
    if (!context || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION)
        || SSL_CTX_load_verify_locations(context, certificate, NULL) != 1)
        fail("client TLS context");

    backend = start_backend(backend_binary, socket_path, "A", backend_ready);
    edge1 = start_edge(edge_binary, certificate, key, socket_path, edge1_ready);
    port1 = read_port(edge1_ready);

    primary = connect_client(context, port1);
    expect_text(&primary, "ATTACHED initial A\n");
    send_bytes(&primary, "before-crash\n", 13);
    expect_text(&primary, "before-crash\n");

    stop_child(backend, SIGKILL);
    backend = start_backend(backend_binary, socket_path, "B", backend_ready);
    expect_text(&primary, "ATTACHED crash B\n");
    send_bytes(&primary, "after-crash\n", 12);
    expect_text(&primary, "after-crash\n");

    snprintf(command, sizeof(command), "BULK %zu\n", bulk);
    send_bytes(&primary, command, strlen(command));
    expect_fill(&primary, 'X', bulk);

    slow = connect_client(context, port1);
    expect_text(&slow, "ATTACHED initial B\n");
    send_bytes(&slow, "BULK 8388608\n", 13);
    short_delay();

    fast = connect_client(context, port1);
    expect_text(&fast, "ATTACHED initial B\n");
    send_bytes(&fast, "fast\n", 5);
    expect_text(&fast, "fast\n");

    edge2 = start_edge(edge_binary, certificate, key, socket_path, edge2_ready);
    port2 = read_port(edge2_ready);
    second_edge = connect_client(context, port2);
    expect_text(&second_edge, "ATTACHED initial B\n");
    send_bytes(&second_edge, "edge-two\n", 9);
    expect_text(&second_edge, "edge-two\n");

    send_malformed(socket_path);
    send_bytes(&fast, "after-malformed\n", 16);
    expect_text(&fast, "after-malformed\n");

    send_bytes(&primary, "GRACEFUL\n", 9);
    if (waitpid(backend, NULL, 0) < 0)
        fail("waiting for graceful backend");
    forget_child(backend);
    backend = start_backend(backend_binary, socket_path, "C", backend_ready);
    expect_text(&primary, "ATTACHED graceful C\n");
    send_bytes(&primary, "after-graceful\n", 15);
    expect_text(&primary, "after-graceful\n");

    expect_text(&fast, "ATTACHED crash C\n");
    expect_text(&second_edge, "ATTACHED crash C\n");

    close_client(&primary);
    close_client(&slow);
    close_client(&fast);
    close_client(&second_edge);
    SSL_CTX_free(context);
    stop_child(backend, SIGTERM);
    stop_child(edge1, SIGTERM);
    stop_child(edge2, SIGTERM);
    puts("integration scenarios passed");
    return 0;
}
