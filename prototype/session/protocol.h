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
#define SP_HELLO_FIXED_SIZE 66u
#define SP_WELCOME_SIZE 32u
#define SP_ACK_SIZE 16u
#define SP_DETACH_SIZE 24u

enum sp_type {
    SP_HELLO = 1,
    SP_WELCOME = 2,
    SP_INPUT = 3,
    SP_OUTPUT = 4,
    SP_ACK = 5,
    SP_DETACH = 6,
    SP_CLOSE = 7,
    SP_ERROR = 8
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
    uint64_t input_position;
    uint64_t output_position;
    uint16_t origin_length;
    const unsigned char *origin;
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

#endif
