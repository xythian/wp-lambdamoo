# Session protocol

Status: protocol 1.0, experimental. The server side is the `session` network
backend (`net_session.c`), selected at configure time and off by default.

This document is the specification. [`Connection_Session_Protocol.md`](Connection_Session_Protocol.md)
explains why the protocol is shaped this way, and
[`prototype/session/`](prototype/session/README.md) holds a C reference edge and
its tests.

## Purpose

With the ordinary TCP backend, a player's network connection and the MOO
process are tied together: when the server process ends, for an upgrade, a
restart, a crash or a panic, every player is disconnected.

The session backend separates them. A separate process, the *edge*, accepts
and owns client connections (plain TCP, TLS, WebSocket or anything else) and
carries each one to the server as a *session* over a private Unix-domain
socket. When the server process is replaced, the edge keeps the client
connection open and attaches the session to the replacement process. A player
who was logged in is resumed as the same player without logging in again, and
the database code that handles logins is not involved.

A session survives replacement of the server. It does not survive loss of the
edge that owns it.

## Building and running the server

```sh
./configure --enable-net=session,poll,require
make
./moo [options] input.db output.db [session-socket-path]
```

The optional last argument is the path of the Unix socket the server listens
on. It defaults to `DEFAULT_CONNECT_FILE` in `options.h`. The server removes
any existing file at that path, binds the socket, and sets its mode to `0600`.

Limits of this backend, compared to the TCP backend:

- **One listener.** The server listens on exactly one socket. Calling
  `listen()` from the database to open another listening point fails with
  `E_INVARG`.
- **No outbound connections.** `OUTBOUND_NETWORK` cannot be defined with this
  backend, so `open_network_connection()` is unavailable. A database that
  makes outbound connections cannot run on it yet.
- **No connection options specific to the carrier.** The options that don't
  depend on the network, such as `binary`, `hold-input` and
  `flush-command`, behave as usual.

## Concepts

**External connection.** The transport between a client and an edge. The edge
owns it, including any TLS or WebSocket state. The server never sees it.

**Session.** The conversation the server represents as a connection. It has a
16-byte opaque ID chosen by the edge, unique across all edges, and lasts as long
as the external connection does.

**Attachment.** One carrier connection binding one session to one server
process. At most one attachment per session exists at a time. Losing an
attachment does not end the session.

**Generation.** A counter that the edge increases each time it replaces a
session's attachment, so that frames from a stale attachment can be told apart.

**Positions.** Each direction of a session is an ordered byte stream. A
position is a byte offset within that stream since the session began,
independent of how bytes are split into frames.

**Binding.** The player object and listener object a session is logged in as.
The server reports the binding to the edge when a player logs in, and the edge
presents it when attaching to a replacement.

## Carrier

Each attachment is one connection to the server's Unix socket, carrying
exactly one session. There is no multiplexing.

The socket's `0600` mode and a peer-credential check are the only
authentication: the server accepts a carrier connection only from a process
with the same effective UID as the server (`SO_PEERCRED`, where available).
Anything that can connect can assert any origin and resume any session the
loaded database records, so the socket must be reachable only by trusted
edges. Session IDs are not credentials.

## Frames

Every frame is a 14-byte header followed by a payload. All integers are
unsigned and big-endian unless a field says otherwise.

| Offset | Size | Field | Value |
| ---: | ---: | --- | --- |
| 0 | 4 | magic | `0x4D534553` (ASCII `MSES`) |
| 4 | 1 | major version | 1 |
| 5 | 1 | minor version | 0 |
| 6 | 1 | type | see below |
| 7 | 1 | reserved | 0 |
| 8 | 2 | flags | 0 |
| 10 | 4 | length | payload length in bytes |

A receiver rejects a frame and ends the attachment if the magic is wrong, the
major version is not 1, the minor version is greater than the receiver's own,
the reserved byte or flags are not zero, the type is unknown, or the length is
not allowed for the type. Readers must accept a frame split across any number
of reads.

| Type | Name | Sent by | Payload length |
| ---: | --- | --- | --- |
| 1 | `HELLO` | edge | 82 to 65536 |
| 2 | `WELCOME` | server | 32 |
| 3 | `INPUT` | edge | 8 to 1048584 |
| 4 | `OUTPUT` | server | 8 to 1048584 |
| 5 | `ACK` | both | 16 |
| 6 | `DETACH` | server | 24 |
| 7 | `CLOSE` | both | 0 to 65536 |
| 8 | `ERROR` | either | 0 to 65536 |
| 9 | `BIND` | server | 16 |

Reserved bytes inside payloads must be zero and are checked by receivers.

### HELLO

The first frame on every attachment. The edge sends it; any other first frame
ends the attachment.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 16 | session ID |
| 16 | 16 | edge instance ID, fresh for each edge process |
| 32 | 8 | attachment generation |
| 40 | 1 | recovery mode: 1 initial, 2 graceful, 3 crash |
| 41 | 7 | reserved |
| 48 | 8 | next input position |
| 56 | 8 | next output position |
| 64 | 8 | bound player (signed), or `INT64_MIN` if not logged in |
| 72 | 8 | bound listener (signed), meaningful only with a player |
| 80 | 2 | origin length |
| 82 | origin length | origin |

The origin is text the edge asserts about the client, such as its address. The
server uses it as the connection's name, so `connection_name()` returns it.
The protocol doesn't define its format.

Recovery mode tells the server why this attachment exists:

- **initial**: a new session. Positions are 0 and there is no binding.
- **graceful**: the previous server detached the session cleanly with
  `DETACH`, at the positions given.
- **crash**: the previous attachment ended without a valid `DETACH`.

### WELCOME

The server's acceptance of a `HELLO`.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | generation, echoed |
| 8 | 1 | recovery mode, echoed |
| 9 | 7 | reserved |
| 16 | 8 | next input position, accepted |
| 24 | 8 | next output position, accepted |

The server accepts the edge's positions as the session's new boundary. If the
server refuses the attachment, it closes the carrier without a `WELCOME`.

### INPUT and OUTPUT

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | position of the first data byte |
| 8 | up to 1 MiB | data |

The position must equal the receiver's next expected position. A gap or an
overlap ends the attachment. Senders may split data across frames however they
like.

### ACK

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | direction: 1 input, 2 output |
| 1 | 7 | reserved |
| 8 | 8 | next position accepted |

The server acknowledges `INPUT` with direction 1, and the edge acknowledges
`OUTPUT` with direction 2. An acknowledged position never moves backwards and
never passes the last position sent. An acknowledgement lets the sender free
its buffer. It says nothing about whether a MOO task ran.

### BIND

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | player (signed) |
| 8 | 8 | listener (signed) |

Sent by the server whenever the connection becomes logged in as a player,
including when it switches player. The edge records the binding and sends it in
every later `HELLO` for this session. A binding is not a credential: the
server checks it against its own database when the session is resumed.

### DETACH

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 1 | mode: 2 graceful |
| 1 | 7 | reserved |
| 8 | 8 | next input position |
| 16 | 8 | next output position |

Sent by the server during an orderly shutdown, after it has flushed all output
for the session. The server then shuts down its side of the carrier. The edge
accepts the detach only if both positions match its own. It then attaches to
the next server in graceful mode at those positions. Any other detach is
treated as a crash.

### CLOSE and ERROR

`CLOSE` ends the session permanently. The server sends it when the database
disconnects the player (`@quit`, `boot_player()`, and so on), and the edge then
closes the client. The edge sends it when the client goes away, and the server
then runs its usual disconnection handling.

`ERROR` rejects a frame or an attachment. The payload of either may be a UTF-8
diagnostic, which is not trusted application data.

## Session lifecycle

### New session

1. The client connects to the edge.
2. The edge connects to the server's socket and sends `HELLO` in initial
   mode, with positions 0, no binding, and the origin.
3. The server sends `WELCOME` and treats the session as a new connection: it
   prints the welcome message and waits for a login command, exactly as for a
   new TCP connection.
4. When a login succeeds, the server sends `BIND`.

### Input

The edge reads from the client and sends `INPUT`. The server processes each
frame as it would bytes read from a TCP socket. In line mode it splits on CR,
LF or CRLF, drops unprintable characters, and carries a UTF-8 sequence split
across frames over to the next frame. In binary mode each frame's data is
delivered to the database as one binary string. After processing a frame, the
server acknowledges it.

When the server suspends input for a connection, it stops reading that
connection's carrier, and the carrier's own backpressure reaches the edge.

### Output

The server sends `OUTPUT` frames, splitting at 1 MiB. When more than
`MAX_QUEUED_OUTPUT` bytes are queued for a session and the carrier can't take
more, output the caller doesn't allow to be flushed is refused. Other output is
queued anyway; this backend doesn't discard old output the way the TCP backend
does. The edge writes output to the client and acknowledges it.

### Ending a session

Either side sends `CLOSE`, as described above. If the carrier ends without
`CLOSE` while the server is running, the server treats the session as ended:
an edge that loses its carrier and still has the client attaches again, while
an edge that has died takes its sessions with it.

### Graceful replacement

A server shutting down in an orderly way (the `shutdown()` builtin, `SIGINT`,
`SIGTERM` or `SIGUSR1`) sends every player the usual `*** Shutting down ***` line and
writes its final database, which records every logged-in player and listener
pair. It then sends each session `DETACH` with
its current positions and closes the carrier. Each edge keeps its clients
connected and retries the socket until a replacement server is listening, then
attaches each session in graceful mode with the binding it holds.

The edge does not read client input while a session is unattached, so nothing
is lost and nothing is replayed.

### Crash recovery

If the server process dies without detaching, each edge sees its carriers end.
For each session it:

1. stops reading client input and discards any input it had read but not sent;
2. never resends input already written to the dead server, whether or not it
   was acknowledged;
3. may still deliver output it had already received to the client;
4. retries the socket, and when a replacement is listening, attaches in crash
   mode with the positions it holds and the binding, if any.

The replacement server may have loaded a database older than the crash, so
anything the dead server did after its last checkpoint may be lost. Tasks that
were waiting in `read()` may not exist.

### Resuming a session

When a `HELLO` carries a binding (graceful or crash mode), the server resumes
the session only if:

- the player is a valid object that isn't already connected, and
- the player and listener pair is in the list of formerly active connections
  recorded in the database the server loaded, and
- the listener still exists.

If those hold, the server takes that pair out of the list, creates the
connection already logged in as the player, logs `SESSION REATTACHED` or
`SESSION RECOVERED`, and sends `WELCOME`. It does not call
`$do_login_command`, `user_connected` or `user_reconnected`, and it doesn't
send a welcome message. After a crash it sends the player the line
`*** Session recovered from a server crash ***`. If any check fails, the
server closes the carrier without `WELCOME`.

A `HELLO` in initial mode that carries a binding is refused. A `HELLO` in
graceful or crash mode without a binding is accepted as a new, not yet logged in
connection.

At startup, the TCP backend tells the database that every connection recorded
in the loaded database has disconnected. The session backend skips that step,
since those players are expected to resume.

### Panics

When the server panics, it writes a PANIC database (`output.db.PANIC`), which
includes the formerly active connections, and exits without detaching sessions
or sending them a shutdown message. Edges see a crash. Restarting the server
from the PANIC database resumes the sessions in crash mode with all state up to
the panic, except the task that was running. Restarting it from an older
checkpoint resumes only the players recorded in that checkpoint.

## Edge obligations

An edge implementing this protocol must:

- choose session IDs that are random and unique across every edge, and a fresh
  edge instance ID for each process;
- increase a session's generation on every new attachment;
- send `HELLO` first and wait for `WELCOME` before sending input;
- keep its input and output positions, and the binding from the most recent
  `BIND`;
- stop reading client input while a session is unattached;
- never replay input after a crash;
- send `CLOSE` when the client goes away, and close the client on `CLOSE`;
- bound every buffer, and pause reading the client when its input allowance is
  exhausted.

How long an edge holds an unattached session, what it tells the client during
the gap, and when it gives up are edge policy, not protocol.

## Versioning

The major version changes only for incompatible changes. A minor version adds
compatible behavior; a receiver accepts frames whose minor version is at most
its own. Flags are reserved for capabilities negotiated in a future minor
version and must be zero in 1.0.

## Implementations

- `net_session.c`: the server.
- `prototype/session/edge.c`: a C reference edge that terminates TLS, with a
  standalone backend harness, codec tests and a full-server integration test.
- The lambdamoo-tests suite contains a Python implementation of the edge side,
  used to test the server.
