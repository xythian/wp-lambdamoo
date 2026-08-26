#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
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
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct tls_client { SSL *ssl; int fd; };
static pid_t children[8];
static size_t child_count;

static void cleanup(void)
{
    size_t i;
    for (i = 0; i < child_count; i++)
        if (children[i] > 0)
            kill(children[i], SIGKILL);
    for (i = 0; i < child_count; i++)
        if (children[i] > 0)
            (void) waitpid(children[i], NULL, 0);
}

static void fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "MOO integration failure: ");
    vfprintf(stderr, format, arguments);
    fprintf(stderr, "\n");
    va_end(arguments);
    cleanup();
    exit(1);
}

static void delay(void)
{
    struct timespec value = {0, 50000000};
    nanosleep(&value, NULL);
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
    if (child_count == sizeof(children) / sizeof(children[0]))
        fail("too many children");
    children[child_count++] = child;
    return child;
}

static void forget(pid_t child)
{
    size_t i;
    for (i = 0; i < child_count; i++)
        if (children[i] == child)
            children[i] = 0;
}

static void stop(pid_t child, int signal_number)
{
    int status;
    if (kill(child, signal_number) < 0)
        fail("kill %ld: %s", (long) child, strerror(errno));
    if (waitpid(child, &status, 0) < 0)
        fail("waitpid %ld: %s", (long) child, strerror(errno));
    forget(child);
    if (signal_number == SIGUSR1 && !WIFEXITED(status))
        fail("MOO did not exit normally after signal %d", signal_number);
}

static void copy_file(const char *source, const char *destination)
{
    unsigned char buffer[65536];
    FILE *in = fopen(source, "rb");
    FILE *out = fopen(destination, "wb");
    size_t count;
    if (!in || !out)
        fail("opening database fixture: %s", strerror(errno));
    while ((count = fread(buffer, 1, sizeof(buffer), in)) != 0)
        if (fwrite(buffer, 1, count, out) != count)
            fail("writing database fixture");
    if (ferror(in) || fclose(in) != 0 || fclose(out) != 0)
        fail("copying database fixture");
}

static int file_contains(const char *path, const char *wanted)
{
    char buffer[8192];
    size_t count;
    FILE *file = fopen(path, "rb");
    if (!file)
        return 0;
    count = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[count] = '\0';
    fclose(file);
    return strstr(buffer, wanted) != NULL;
}

static void wait_for_file(const char *path)
{
    struct stat status;
    unsigned i;
    for (i = 0; i < 200; i++) {
        if (stat(path, &status) == 0)
            return;
        delay();
    }
    fail("timed out waiting for %s", path);
}

static void wait_for_log(const char *path, const char *text)
{
    unsigned i;
    for (i = 0; i < 200; i++) {
        if (file_contains(path, text))
            return;
        delay();
    }
    fail("timed out waiting for '%s' in %s", text, path);
}

static unsigned read_port(const char *path)
{
    unsigned port;
    FILE *file = fopen(path, "r");
    if (!file || fscanf(file, "%u", &port) != 1 || fclose(file) != 0)
        fail("reading edge port");
    return port;
}

static pid_t start_moo(const char *binary, const char *log, const char *input,
                       const char *output, const char *socket_path)
{
    char *arguments[] = {(char *) binary, "-l", (char *) log, (char *) input,
                         (char *) output, (char *) socket_path, NULL};
    pid_t child;
    unlink(socket_path);
    unlink(log);
    child = spawn(arguments);
    wait_for_log(log, "LISTEN:");
    return child;
}

static pid_t start_edge(const char *binary, const char *certificate,
                        const char *key, const char *socket_path,
                        const char *ready)
{
    char *arguments[] = {(char *) binary, (char *) certificate, (char *) key,
                         (char *) socket_path, "127.0.0.1", "0",
                         (char *) ready, NULL};
    unlink(ready);
    pid_t child = spawn(arguments);
    wait_for_file(ready);
    return child;
}

static struct tls_client connect_client(SSL_CTX *context, unsigned port)
{
    struct sockaddr_in address;
    struct timeval timeout = {10, 0};
    struct tls_client client;
    client.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client.fd < 0)
        fail("client socket");
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
        fail("verified TLS connection");
    }
    return client;
}

static void send_text(struct tls_client *client, const char *text)
{
    size_t done = 0, length = strlen(text);
    while (done < length) {
        size_t count = 0;
        if (SSL_write_ex(client->ssl, text + done, length - done, &count) != 1)
            fail("TLS write");
        done += count;
    }
}

static void expect_text(struct tls_client *client, const char *text)
{
    unsigned char buffer[4096];
    size_t done = 0, length = strlen(text);
    while (done < length) {
        size_t count = 0;
        if (SSL_read_ex(client->ssl, buffer, length - done, &count) != 1
            || count == 0)
            fail("TLS read after %zu of %zu bytes", done, length);
        if (memcmp(buffer, text + done, count) != 0)
            fail("unexpected TLS output at byte %zu", done);
        done += count;
    }
}

int main(int argc, char **argv)
{
    const char *moo, *edge, *fixture, *certificate, *key, *directory;
    char input[256], checkpoint1[256], checkpoint2[256], checkpoint3[256];
    char socket_path[256], ready[256], log1[256], log2[256], log3[256];
    SSL_CTX *context;
    struct tls_client client;
    pid_t server, edge_process;

    if (argc != 7) {
        fprintf(stderr, "usage: %s MOO EDGE DB CERT KEY TMPDIR\n", argv[0]);
        return 2;
    }
    moo = argv[1]; edge = argv[2]; fixture = argv[3];
    certificate = argv[4]; key = argv[5]; directory = argv[6];
#define PATH(name, leaf) snprintf(name, sizeof(name), "%s/%s", directory, leaf)
    PATH(input, "moo-input.db"); PATH(checkpoint1, "moo-checkpoint-1.db");
    PATH(checkpoint2, "moo-checkpoint-2.db"); PATH(checkpoint3, "moo-checkpoint-3.db");
    PATH(socket_path, "moo-backend.sock"); PATH(ready, "moo-edge.ready");
    PATH(log1, "moo-1.log"); PATH(log2, "moo-2.log"); PATH(log3, "moo-3.log");
#undef PATH
    unlink(checkpoint1); unlink(checkpoint2); unlink(checkpoint3);
    copy_file(fixture, input);
    signal(SIGPIPE, SIG_IGN);
    context = SSL_CTX_new(TLS_client_method());
    if (!context || !SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION)
        || SSL_CTX_load_verify_locations(context, certificate, NULL) != 1)
        fail("client TLS context");

    server = start_moo(moo, log1, input, checkpoint1, socket_path);
    edge_process = start_edge(edge, certificate, key, socket_path, ready);
    client = connect_client(context, read_port(ready));
    expect_text(&client, "*** Connected ***\r\n");
    wait_for_log(log1, "CONNECTED: Wizard (#3)");

    if (kill(server, SIGUSR2) < 0)
        fail("requesting checkpoint");
    wait_for_log(log1, "CHECKPOINTING on");
    wait_for_log(log1, "finished");
    wait_for_file(checkpoint1);
    stop(server, SIGKILL);

    server = start_moo(moo, log2, checkpoint1, checkpoint2, socket_path);
    expect_text(&client, "*** Session recovered from a server crash ***\r\n");
    wait_for_log(log2, "SESSION RECOVERED: #3");
    send_text(&client, "after crash\n");
    expect_text(&client, "I couldn't understand that.\r\n");

    delay();
    stop(server, SIGUSR1);
    wait_for_file(checkpoint2);
    expect_text(&client, "*** Shutting down: shutdown signal received ***\r\n");
    server = start_moo(moo, log3, checkpoint2, checkpoint3, socket_path);
    wait_for_log(log3, "SESSION REATTACHED: #3");
    send_text(&client, "after graceful\n");
    expect_text(&client, "I couldn't understand that.\r\n");

    SSL_shutdown(client.ssl);
    SSL_free(client.ssl);
    close(client.fd);
    SSL_CTX_free(context);
    stop(server, SIGUSR1);
    stop(edge_process, SIGTERM);
    puts("full MOO session integration passed");
    return 0;
}
