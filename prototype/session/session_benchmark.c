#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SESSION_COUNT 100
#define RTT_COUNT 500
#define BULK_SIZE (4u * 1024u * 1024u)
#define BULK_COUNT 5

struct client { SSL *ssl; int fd; };
static pid_t backend_pid, edge_pid;

static void cleanup(void)
{
    if (backend_pid > 0) kill(backend_pid, SIGKILL);
    if (edge_pid > 0) kill(edge_pid, SIGKILL);
    if (backend_pid > 0) (void) waitpid(backend_pid, NULL, 0);
    if (edge_pid > 0) (void) waitpid(edge_pid, NULL, 0);
}

static void fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "benchmark failure: ");
    vfprintf(stderr, format, arguments);
    fprintf(stderr, "\n");
    va_end(arguments);
    cleanup();
    exit(1);
}

static double now_seconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        fail("clock_gettime");
    return value.tv_sec + value.tv_nsec / 1000000000.0;
}

static void wait_file(const char *path)
{
    struct stat status;
    struct timespec delay = {0, 10000000};
    unsigned i;
    for (i = 0; i < 1000; i++) {
        if (stat(path, &status) == 0)
            return;
        nanosleep(&delay, NULL);
    }
    fail("waiting for %s", path);
}

static pid_t spawn(char *const arguments[])
{
    pid_t child = fork();
    if (child < 0)
        fail("fork: %s", strerror(errno));
    if (child == 0) {
        execv(arguments[0], arguments);
        perror("execv");
        _exit(127);
    }
    return child;
}

static unsigned read_port(const char *path)
{
    unsigned port;
    FILE *file = fopen(path, "r");
    if (!file || fscanf(file, "%u", &port) != 1 || fclose(file) != 0)
        fail("reading port");
    return port;
}

static struct client connect_client(SSL_CTX *context, unsigned port)
{
    struct sockaddr_in address;
    struct timeval timeout = {10, 0};
    struct client client;
    client.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client.fd < 0)
        fail("socket");
    setsockopt(client.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(client.fd, (struct sockaddr *) &address, sizeof(address)) < 0)
        fail("connect: %s", strerror(errno));
    client.ssl = SSL_new(context);
    if (!client.ssl || SSL_set1_host(client.ssl, "localhost") != 1
        || SSL_set_fd(client.ssl, client.fd) != 1
        || SSL_connect(client.ssl) != 1
        || SSL_get_verify_result(client.ssl) != X509_V_OK)
        fail("TLS connect");
    return client;
}

static void send_all(struct client *client, const void *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        size_t count = 0;
        if (SSL_write_ex(client->ssl, (const unsigned char *) data + done,
                         length - done, &count) != 1)
            fail("TLS write");
        done += count;
    }
}

static void receive_exact(struct client *client, const void *expected,
                          size_t length)
{
    unsigned char buffer[65536];
    const unsigned char *wanted = expected;
    size_t done = 0;
    while (done < length) {
        size_t count = 0;
        size_t request = length - done > sizeof(buffer)
            ? sizeof(buffer) : length - done;
        if (SSL_read_ex(client->ssl, buffer, request, &count) != 1 || !count)
            fail("TLS read");
        if (memcmp(buffer, wanted + done, count) != 0)
            fail("data mismatch at %zu", done);
        done += count;
    }
}

static void receive_fill(struct client *client, unsigned char value,
                         size_t length)
{
    unsigned char buffer[65536];
    size_t done = 0;
    while (done < length) {
        size_t count = 0, i;
        size_t request = length - done > sizeof(buffer)
            ? sizeof(buffer) : length - done;
        if (SSL_read_ex(client->ssl, buffer, request, &count) != 1 || !count)
            fail("bulk TLS read");
        for (i = 0; i < count; i++)
            if (buffer[i] != value)
                fail("bulk mismatch at %zu", done + i);
        done += count;
    }
}

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *) left, b = *(const double *) right;
    return a < b ? -1 : a > b;
}

int main(int argc, char **argv)
{
    const char *edge, *backend, *certificate, *key, *directory;
    char socket_path[256], backend_ready[256], edge_ready[256], bulk_command[64];
    char *backend_args[5], *edge_args[9];
    struct client clients[SESSION_COUNT];
    double samples[RTT_COUNT], bulk_samples[BULK_COUNT], started, elapsed;
    SSL_CTX *context;
    unsigned port, i;
    static const char banner[] = "ATTACHED initial bench\n";
    static const char ping[] = "0123456789abcdef0123456789abcde\n";

    if (argc != 6) {
        fprintf(stderr, "usage: %s EDGE BACKEND CERT KEY TMPDIR\n", argv[0]);
        return 2;
    }
    edge = argv[1]; backend = argv[2]; certificate = argv[3];
    key = argv[4]; directory = argv[5];
    snprintf(socket_path, sizeof(socket_path), "%s/bench-backend.sock", directory);
    snprintf(backend_ready, sizeof(backend_ready), "%s/bench-backend.ready", directory);
    snprintf(edge_ready, sizeof(edge_ready), "%s/bench-edge.ready", directory);
    unlink(backend_ready); unlink(edge_ready);
    backend_args[0] = (char *) backend; backend_args[1] = socket_path;
    backend_args[2] = "bench"; backend_args[3] = backend_ready; backend_args[4] = NULL;
    edge_args[0] = (char *) edge; edge_args[1] = (char *) certificate;
    edge_args[2] = (char *) key; edge_args[3] = socket_path; edge_args[4] = "127.0.0.1";
    edge_args[5] = "0"; edge_args[6] = edge_ready; edge_args[7] = NULL;
    signal(SIGPIPE, SIG_IGN);
    backend_pid = spawn(backend_args); wait_file(backend_ready);
    edge_pid = spawn(edge_args); wait_file(edge_ready); port = read_port(edge_ready);
    context = SSL_CTX_new(TLS_client_method());
    if (!context || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION)
        || SSL_CTX_load_verify_locations(context, certificate, NULL) != 1)
        fail("TLS context");

    started = now_seconds();
    for (i = 0; i < SESSION_COUNT; i++) {
        clients[i] = connect_client(context, port);
        receive_exact(&clients[i], banner, sizeof(banner) - 1);
    }
    elapsed = now_seconds() - started;
    printf("sessions=%u setup_total_ms=%.3f setup_mean_ms=%.3f\n",
           SESSION_COUNT, elapsed * 1000.0,
           elapsed * 1000.0 / SESSION_COUNT);

    for (i = 0; i < RTT_COUNT; i++) {
        started = now_seconds();
        send_all(&clients[0], ping, sizeof(ping) - 1);
        receive_exact(&clients[0], ping, sizeof(ping) - 1);
        samples[i] = (now_seconds() - started) * 1000.0;
    }
    qsort(samples, RTT_COUNT, sizeof(samples[0]), compare_double);
    printf("rtt_samples=%u rtt_p50_ms=%.3f rtt_p95_ms=%.3f rtt_p99_ms=%.3f\n",
           RTT_COUNT, samples[RTT_COUNT / 2], samples[RTT_COUNT * 95 / 100],
           samples[RTT_COUNT * 99 / 100]);

    snprintf(bulk_command, sizeof(bulk_command), "BULK %u\n", BULK_SIZE);
    for (i = 0; i < BULK_COUNT; i++) {
        started = now_seconds();
        send_all(&clients[0], bulk_command, strlen(bulk_command));
        receive_fill(&clients[0], 'X', BULK_SIZE);
        bulk_samples[i] = (now_seconds() - started) * 1000.0;
    }
    qsort(bulk_samples, BULK_COUNT, sizeof(bulk_samples[0]), compare_double);
    elapsed = bulk_samples[BULK_COUNT / 2] / 1000.0;
    printf("bulk_bytes=%u bulk_samples=%u bulk_p50_ms=%.3f bulk_p100_ms=%.3f bulk_p50_MiB_s=%.3f\n",
           BULK_SIZE, BULK_COUNT, bulk_samples[BULK_COUNT / 2],
           bulk_samples[BULK_COUNT - 1], (BULK_SIZE / 1048576.0) / elapsed);

    for (i = 0; i < SESSION_COUNT; i++) {
        SSL_shutdown(clients[i].ssl); SSL_free(clients[i].ssl); close(clients[i].fd);
    }
    SSL_CTX_free(context);
    cleanup(); backend_pid = edge_pid = 0;
    return 0;
}
