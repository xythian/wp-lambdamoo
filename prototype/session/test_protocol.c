#include "protocol.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void
test_u64(void)
{
    unsigned char bytes[8];
    uint64_t value = UINT64_C(0x0123456789abcdef);

    sp_put_u64(bytes, value);
    assert(sp_get_u64(bytes) == value);
}

static void
test_hello(void)
{
    struct sp_hello source;
    struct sp_hello decoded;
    struct sp_frame frame;
    unsigned char *payload;
    uint32_t length;
    const unsigned char origin[] = "tls:127.0.0.1:1234";

    memset(&source, 0, sizeof(source));
    memset(source.session_id, 0x11, sizeof(source.session_id));
    memset(source.edge_id, 0x22, sizeof(source.edge_id));
    source.generation = 17;
    source.mode = SP_MODE_CRASH;
    source.input_position = 1234;
    source.output_position = 5678;
    source.player = 3;
    source.listener = 0;
    source.origin = origin;
    source.origin_length = (uint16_t) (sizeof(origin) - 1);

    assert(sp_encode_hello(&source, &payload, &length) == 0);
    frame.type = SP_HELLO;
    frame.flags = 0;
    frame.length = length;
    frame.payload = payload;
    assert(sp_decode_hello(&frame, &decoded) == 0);
    assert(memcmp(decoded.session_id, source.session_id, SP_ID_SIZE) == 0);
    assert(memcmp(decoded.edge_id, source.edge_id, SP_ID_SIZE) == 0);
    assert(decoded.generation == source.generation);
    assert(decoded.mode == source.mode);
    assert(decoded.input_position == source.input_position);
    assert(decoded.output_position == source.output_position);
    assert(decoded.player == source.player);
    assert(decoded.listener == source.listener);
    assert(decoded.origin_length == source.origin_length);
    assert(memcmp(decoded.origin, origin, source.origin_length) == 0);
    free(payload);
}

static void
test_frame_round_trip(void)
{
    int sockets[2];
    pid_t child;
    struct sp_frame frame;
    unsigned char data[11];
    int status;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        sp_put_u64(data, 91);
        memcpy(data + 8, "abc", 3);
        _exit(sp_frame_write(sockets[1], SP_INPUT, 0, data, sizeof(data)) == 1
              ? 0 : 1);
    }
    close(sockets[1]);
    assert(sp_frame_read(sockets[0], &frame) == 1);
    assert(frame.type == SP_INPUT);
    assert(frame.length == sizeof(data));
    assert(sp_get_u64(frame.payload) == 91);
    assert(memcmp(frame.payload + 8, "abc", 3) == 0);
    sp_frame_clear(&frame);
    assert(sp_frame_read(sockets[0], &frame) == 0);
    close(sockets[0]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
test_rejects_bad_header(void)
{
    int sockets[2];
    unsigned char header[SP_HEADER_SIZE] = {0};
    struct sp_frame frame;

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(write(sockets[0], header, sizeof(header)) == (ssize_t) sizeof(header));
    close(sockets[0]);
    assert(sp_frame_read(sockets[1], &frame) == -2);
    close(sockets[1]);
}

static void
test_codecs(void)
{
    struct sp_frame frame;
    struct sp_welcome welcome = {9, SP_MODE_GRACEFUL, 10, 20};
    struct sp_welcome decoded_welcome;
    struct sp_ack ack = {SP_DIR_OUTPUT, 99};
    struct sp_ack decoded_ack;
    struct sp_detach detach = {SP_MODE_CRASH, 7, 8};
    struct sp_detach decoded_detach;
    unsigned char welcome_bytes[SP_WELCOME_SIZE];
    unsigned char ack_bytes[SP_ACK_SIZE];
    unsigned char detach_bytes[SP_DETACH_SIZE];

    struct sp_bind bind = {3, 0};
    struct sp_bind decoded_bind;
    unsigned char bind_bytes[SP_BIND_SIZE];
    sp_encode_welcome(&welcome, welcome_bytes);
    frame = (struct sp_frame) {SP_WELCOME, 0, SP_WELCOME_SIZE, welcome_bytes};
    assert(sp_decode_welcome(&frame, &decoded_welcome) == 0);
    assert(decoded_welcome.generation == 9);
    assert(decoded_welcome.mode == SP_MODE_GRACEFUL);
    assert(decoded_welcome.input_position == 10);
    assert(decoded_welcome.output_position == 20);

    sp_encode_ack(&ack, ack_bytes);
    frame = (struct sp_frame) {SP_ACK, 0, SP_ACK_SIZE, ack_bytes};
    assert(sp_decode_ack(&frame, &decoded_ack) == 0);
    assert(decoded_ack.direction == SP_DIR_OUTPUT);
    assert(decoded_ack.position == 99);

    ack_bytes[1] = 1;
    assert(sp_decode_ack(&frame, &decoded_ack) < 0);
    ack_bytes[1] = 0;
    assert(sp_frame_write(-1, SP_ACK, 1, ack_bytes, sizeof(ack_bytes)) < 0);

    sp_encode_detach(&detach, detach_bytes);
    frame = (struct sp_frame) {SP_DETACH, 0, SP_DETACH_SIZE, detach_bytes};
    assert(sp_decode_detach(&frame, &decoded_detach) == 0);
    assert(decoded_detach.mode == SP_MODE_CRASH);
    assert(decoded_detach.input_position == 7);
    assert(decoded_detach.output_position == 8);
    sp_encode_bind(&bind, bind_bytes);
    frame = (struct sp_frame) {SP_BIND, 0, SP_BIND_SIZE, bind_bytes};
    assert(sp_decode_bind(&frame, &decoded_bind) == 0);
    assert(decoded_bind.player == 3);
    assert(decoded_bind.listener == 0);

}

int
main(void)
{
    test_u64();
    test_hello();
    test_frame_round_trip();
    test_rejects_bad_header();
    test_codecs();
    puts("protocol tests passed");
    return 0;
}
