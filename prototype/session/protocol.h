#ifndef WP_SESSION_PROTOCOL_H
#define WP_SESSION_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define SP_MAGIC 0x4d534553u
#define SP_VERSION_MAJOR 1u
#define SP_VERSION_MINOR 0u
#define SP_HEADER_SIZE 14u
#define SP_MAX_CONTROL (64u * 1024u)
#define SP_MAX_DATA (1024u * 1024u)
#define SP_MAX_PAYLOAD (SP_MAX_DATA + 8u)
#define SP_ID_SIZE 16u
#define SP_HELLO_FIXED_SIZE 82u
#define SP_BIND_SIZE 16u
#define SP_WELCOME_SIZE 32u
#define SP_ACK_SIZE 16u
#define SP_DETACH_SIZE 24u
#define SP_CONNECT_FIXED_SIZE 16u
#define SP_CONNECT_FAILED_FIXED_SIZE 8u
#define SP_MAX_LISTENERS 1024u
#define SP_LISTEN_FIXED_SIZE 10u

enum sp_type {
    SP_HELLO = 1,
    SP_WELCOME = 2,
    SP_INPUT = 3,
    SP_OUTPUT = 4,
    SP_ACK = 5,
    SP_DETACH = 6,
    SP_CLOSE = 7,
    SP_ERROR = 8,
    SP_BIND = 9,
    SP_LISTENERS = 10,
    SP_CONNECT = 11,
    SP_CONNECT_FAILED = 12
};

enum sp_kind {
    SP_KIND_CLIENT = 0,
    SP_KIND_OUTBOUND = 1,
    SP_KIND_CONTROL = 2
};

enum sp_connect_flag {
    SP_CONNECT_TLS = 1
};

enum sp_listen_flag {
    SP_LISTEN_TLS = 1,
    SP_LISTEN_ACME = 2
};

enum sp_connect_failure {
    SP_FAILED_DESTINATION = 1,
    SP_FAILED_RESOURCES = 2
};

enum sp_mode {
    SP_MODE_INITIAL = 1,
    SP_MODE_GRACEFUL = 2,
    SP_MODE_CRASH = 3
};

enum sp_direction {
    SP_DIR_INPUT = 1,
    SP_DIR_OUTPUT = 2
};

struct sp_frame {
    uint8_t type;
    uint16_t flags;
    uint32_t length;
    unsigned char *payload;
};

struct sp_hello {
    unsigned char session_id[SP_ID_SIZE];
    unsigned char edge_id[SP_ID_SIZE];
    uint64_t generation;
    uint8_t mode;
    uint8_t kind;
    uint16_t listen_port;
    uint32_t request;
    uint64_t input_position;
    uint64_t output_position;
    int64_t player;
    int64_t listener;
    uint16_t origin_length;
    const unsigned char *origin;
};

struct sp_listen {
    uint16_t port;
    uint16_t flags;
    const char *certificate;	/* null for the edge's own or ACME */
    const char *key;
    const char *acme;		/* comma-separated names, or null */
};

struct sp_connect {
    uint32_t request;
    uint16_t port;
    uint16_t flags;
    uint32_t timeout;
    uint16_t host_length;
    const unsigned char *host;
};

struct sp_connect_failed {
    uint32_t request;
    uint8_t code;
    uint32_t reason_length;
    const unsigned char *reason;
};

struct sp_welcome {
    uint64_t generation;
    uint8_t mode;
    uint64_t input_position;
    uint64_t output_position;
};

struct sp_ack {
    uint8_t direction;
    uint64_t position;
};

struct sp_detach {
    uint8_t mode;
    uint64_t input_position;
    uint64_t output_position;
};

uint64_t sp_get_u64(const unsigned char *p);
struct sp_bind {
    int64_t player;
    int64_t listener;
};

void sp_put_u64(unsigned char *p, uint64_t value);

int sp_frame_read(int fd, struct sp_frame *frame);
int sp_frame_write(int fd, uint8_t type, uint16_t flags,
                   const void *payload, uint32_t length);
void sp_frame_clear(struct sp_frame *frame);

int sp_encode_hello(const struct sp_hello *hello, unsigned char **payload,
                    uint32_t *length);
int sp_decode_hello(const struct sp_frame *frame, struct sp_hello *hello);
void sp_encode_welcome(const struct sp_welcome *welcome,
                       unsigned char payload[SP_WELCOME_SIZE]);
int sp_decode_welcome(const struct sp_frame *frame, struct sp_welcome *welcome);
void sp_encode_ack(const struct sp_ack *ack,
                   unsigned char payload[SP_ACK_SIZE]);
int sp_decode_ack(const struct sp_frame *frame, struct sp_ack *ack);
void sp_encode_detach(const struct sp_detach *detach,
                      unsigned char payload[SP_DETACH_SIZE]);
int sp_decode_detach(const struct sp_frame *frame, struct sp_detach *detach);
void sp_encode_bind(const struct sp_bind *bind,
                    unsigned char payload[SP_BIND_SIZE]);
int sp_decode_bind(const struct sp_frame *frame, struct sp_bind *bind);
int sp_encode_listeners(const struct sp_listen *listens, unsigned count,
                        unsigned char **payload, uint32_t *length);
int sp_encode_connect(const struct sp_connect *connect,
                      unsigned char **payload, uint32_t *length);
int sp_decode_connect(const struct sp_frame *frame, struct sp_connect *connect);
int sp_encode_connect_failed(const struct sp_connect_failed *failed,
                             unsigned char **payload, uint32_t *length);
int sp_decode_connect_failed(const struct sp_frame *frame,
                             struct sp_connect_failed *failed);

#endif
