# Session carrier protocol 1.0

Status: prototype wire contract.

This protocol binds one authenticated byte stream to one MOO session attachment.
It deliberately does not multiplex sessions and does not define transport
security. The initial implementation uses Unix-domain sockets protected by
filesystem permissions and peer credentials.

All integers are unsigned and in network byte order. Reserved bytes and flags
must be zero when sent and must be rejected when their meaning is required for
safe processing. Readers operate incrementally and must accept arbitrary stream
read boundaries.

## Frame

Every frame begins with a 12-byte header:

| Field | Size | Meaning |
| --- | ---: | --- |
| magic | 4 | ASCII-compatible value `MSES` |
| major | 1 | incompatible protocol version, initially 1 |
| minor | 1 | compatible protocol revision, initially 0 |
| type | 1 | required operation |
| reserved | 1 | zero |
| flags | 2 | zero unless negotiated |
| length | 4 | payload bytes following the header |

Control payloads are limited to 64 KiB. Input and output contain an eight-byte
position followed by at most 1 MiB of opaque data. Invalid magic, version,
length, reserved fields, flags, or required type terminates the attachment.

## Attachment handshake

The edge sends `HELLO` first:

| Field | Size |
| --- | ---: |
| session ID | 16 |
| edge instance ID | 16 |
| attachment generation | 8 |
| recovery mode | 1 |
| reserved | 7 |
| next input position | 8 |
| next output position | 8 |
| bound player | 8 |
| listener object | 8 |
| origin length | 2 |
| trusted origin metadata | origin length |

IDs are opaque. Session IDs are globally unique. Edge instance IDs are fresh
for each edge process. The generation increases whenever that edge replaces the
attachment. Recovery mode is initial, graceful, or crash.

The bound player is the signed 64-bit MOO object number, or `INT64_MIN` for a
new, unbound session. The listener is meaningful only for a bound player. A
replacement server authorizes a resume by matching both values against the
formerly active connections in its loaded checkpoint.

The backend authenticates and authorizes the carrier before trusting the IDs or
origin. It answers with `WELCOME`, containing generation, mode, seven reserved
bytes, and the accepted next input and output positions. A mismatch terminates
the attachment.

After a crash, positions establish a new no-replay boundary; they do not claim
that the failed server processed input or that its state survived. After a
graceful replacement, they describe the quiesced delivery boundary.

After login or another in-server connection reassignment, the backend sends
`BIND`, containing the signed 64-bit player and listener object numbers. The
edge retains this binding and includes it in later `HELLO` frames. `BIND` does
not itself authorize a future resume; the secured carrier and checkpoint match
do that.

## Data and ownership

`INPUT` and `OUTPUT` carry an eight-byte starting position followed by opaque
bytes. Positions count bytes within the session and do not depend on frame
boundaries. A receiver rejects gaps and duplicates in protocol 1.0.

`ACK` contains a one-byte direction, seven reserved bytes, and the next byte
position accepted by the receiving session layer. It transfers ownership for
buffer reclamation. It says nothing about MOO task creation or completion and
must not cause input replay.

The prototype bounds each data frame at 1 MiB. Implementations may impose
smaller queue limits and exert carrier backpressure.

## Detach and close

`DETACH` contains recovery mode, seven reserved bytes, and next input and
output positions. Graceful detach is sent only after the sender has quiesced at
the stated boundary. Carrier loss without a valid graceful detach is a crash.

`CLOSE` permanently ends the session and may contain a UTF-8 diagnostic.
`ERROR` rejects an attachment or frame and may contain a UTF-8 diagnostic.
Neither diagnostic is trusted application data.

An edge never replays input from a failed attachment. It discards input already
buffered specifically for that attachment, stops reading additional external
input while unattached, and signals crash recovery when a replacement attaches.
Output already accepted from the old backend may be drained to the client.
