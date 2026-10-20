# Session protocol

Status: protocol 1.0, experimental. The server side is the `session` network
backend (`net_session.c`), selected at configure time and off by default.

This document is the specification. [`Connection_Session_Protocol.md`](Connection_Session_Protocol.md)
explains why the protocol is shaped this way, and
[`prototype/session/`](prototype/session/README.md) holds a C reference edge and
its tests.

[`session_protocol.ksy`](session_protocol.ksy) describes every frame's byte
layout in [Kaitai Struct](https://kaitai.io/), from which parsers can be
generated for most languages. It is normative for byte layouts, and the
tables below follow it; this document is normative for everything else.

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
the database code that handles logins is not involved. That happens only when
the replacement loads exactly the database the old process wrote as it let go
of the session: its shutdown dump, or its PANIC dump after a panic. After a
crash with no such dump, logged-in players are disconnected as before.

A session survives replacement of the server. It does not survive loss of the
edge that owns it.

## Building and running the server

```sh
./configure --enable-net=session,poll,outoff,require
make
./moo [options] input.db output.db [+O|-O] [-t tls-port] [session-socket-path]
```

The optional last argument is the path of the Unix socket the server listens
on. It defaults to `DEFAULT_CONNECT_FILE` in `options.h`. The server removes
any existing file at that path, binds the socket, and sets its mode to `0600`.
`outoff` compiles in outbound connections, disabled unless the server is
started with `+O`; `out` enables them unless it is started with `-O`, and
leaving both out removes `open_network_connection()`, as with the TCP backend.

The socket is the listener for connections the edge accepts on its own port,
and is the listener object `#0`'s initial listening point, as `-p` gives the TCP
backend. `-t tls-port` also has the edge accept TLS clients for `#0` on that
port, with the edge's own certificate (which the edge may itself get by ACME,
as its own configuration says). Everything else the database does with
the network goes through the edge:

- **`listen(object, port [, print-messages] [, options])`** asks the edge to
  accept clients on that TCP port for the server
  ([Listening ports](#listening-ports)). It returns the port. The port must be
  given (1 to 65535); the edge doesn't choose one, so `listen(object, 0)`
  raises `E_INVARG`, and a point that isn't an integer raises `E_TYPE`. With
  the `tls` option the edge accepts TLS on the port. `unlisten(port)` asks the
  edge to stop.
- **`open_network_connection(host, port [, listener] [, options])`** asks the
  edge to make the connection ([Outbound connections](#outbound-connections)).
  The calling task is suspended while the edge connects, and other tasks run
  in the meantime. With the `tls` option the edge connects with TLS, verifying
  the host's certificate against the name given. Failures raise what the TCP
  backend raises: `E_INVARG` for a host or port that can't be reached
  (including a failed TLS handshake), `E_QUOTA` for the edge running out of
  resources or losing its control attachment, and `E_PERM` when outbound
  connections are disabled.

### Network options

*Options* is a list of `{name, value}` pairs, and is the same for every
network backend, so a database written for one runs on another. Names are
case-insensitive.

| Builtin | Name | Value |
|---|---|---|
| both | `tls` | true for TLS |
| `listen()` | `print-messages` | the same as the `print-messages` argument, which it overrides |
| `listen()` | `certificate` | path of the certificate chain file to accept TLS with |
| `listen()` | `key` | path of the private key file for `certificate` |
| `listen()` | `acme` | a host name, or a list of them, to get a certificate for by ACME |
| `open_network_connection()` | `listener` | the same as the `listener` argument |

An option name the builtin doesn't know, an entry that isn't a
`{string, value}` pair, `certificate` or `key` without the other or without
`tls`, `acme` without `tls` or with `certificate` and `key`, or `listener`
given twice raises `E_INVARG`; an options argument that isn't a list, or a
value of the wrong type, raises `E_TYPE`. Without `certificate` and `key`, a
TLS port uses the edge's own certificate. Paths are the edge's: it reads the
files, and the server never does.

With `acme`, the edge gets and renews a certificate covering the names from an
ACME certificate authority (such as Let's Encrypt), and accepts TLS on the port
with it. Each name is a DNS name or a wildcard one: 1 to 253 letters, digits,
`-`, `.` and `*`; an empty list, more than 100 names, or any other character
raises `E_INVARG`. Which authority, account and challenge the edge uses are its
own configuration, not the database's. An edge without an ACME client treats
the port as one whose certificate it can't load.

A backend that can't do TLS raises `E_INVARG` for a true `tls` rather than
making a plain connection; today that is every backend but this one. A TCP
backend that terminated TLS itself would take the same options and use the
same `-t` switch, so a database needs no change to move between the two.

Limits of this backend, compared to the TCP backend:

- **Connections that aren't logged in end with the server.** An outbound
  connection, or a client on any port, that is not logged in as a player when
  the server is replaced is closed (outbound) or offered a fresh login
  (inbound), as described under [Resuming a session](#resuming-a-session).
- **No connection options specific to the carrier.** The options that don't
  depend on the network, such as `binary`, `hold-input` and
  `flush-command`, behave as usual.
- **A task waiting in `open_network_connection()` is not checkpointed** with
  that wait; if the server is replaced while the edge is connecting, the task
  is gone and the edge's connection is refused and closed.

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

**Kind.** Each attachment says what it carries: a *client* session the edge
accepted, an *outbound* session the edge opened because the server asked, or
the edge's *control* attachment.

**Control attachment.** Each edge keeps one attachment that carries no session.
The server uses it to tell the edge which ports to listen on and which
outbound connections to make. The edge reopens it whenever it ends.

## Carrier

Each attachment is one connection to the server's Unix socket, carrying
exactly one session. There is no multiplexing.

The socket's `0600` mode and a peer-credential check are the only
authentication: the server accepts a carrier connection only from a process
with the same effective UID as the server (`SO_PEERCRED`, where available).
Anything that can connect can describe any client and resume any session the
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
| 1 | `HELLO` | edge | 98 to 65536 |
| 2 | `WELCOME` | server | 32 |
| 3 | `INPUT` | edge | 8 to 1048584 |
| 4 | `OUTPUT` | server | 8 to 1048584 |
| 5 | `ACK` | both | 16 |
| 6 | `DETACH` | server | 40 |
| 7 | `CLOSE` | both | 0 to 65536 |
| 8 | `ERROR` | either | 0 to 65536 |
| 9 | `BIND` | server | 16 |
| 10 | `LISTENERS` | server, control only | 0 to 65536 |
| 11 | `CONNECT` | server, control only | 17 to 65536 |
| 12 | `CONNECT_FAILED` | edge, control only | 8 to 65536 |
| 13 | `LISTEN_FAILED` | edge, control only | 8 to 65536 |

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
| 41 | 1 | kind: 0 client, 1 outbound, 2 control |
| 42 | 2 | listen port: the edge port a client arrived on, or 0 for the edge's own |
| 44 | 4 | request: the `CONNECT` an outbound session answers, or 0 |
| 48 | 8 | next input position |
| 56 | 8 | next output position |
| 64 | 8 | bound player (signed), or `INT64_MIN` if not logged in |
| 72 | 8 | bound listener (signed), meaningful only with a player |
| 80 | 16 | database: the digest from the session's last `DETACH`, or zeros |
| 96 | 2 | connection fields length |
| 98 | fields length | connection fields |

#### Connection fields

The connection fields are what the edge asserts about how the client reached
it. They fill the rest of the `HELLO` exactly, as a sequence of:

| Size | Field |
| ---: | --- |
| 1 | name length, 1 to 32 |
| name length | name: lowercase ASCII letters, digits and `-` |
| 1 | type: 1 string, 2 integer |
| 2 | string length, for a string |
| string length | string: printable ASCII (0x20 to 0x7e) |
| 8 | signed value, for an integer |

A `HELLO` may carry at most 32 fields, and no name twice. A client or outbound
session must carry these, with these types:

| Name | Type | Meaning |
| --- | --- | --- |
| `transport` | string | how the client reached the edge, such as `tcp`, `tls`, `websocket` or `ssh` |
| `local-port` | integer, 0 to 65535 | the edge's port on the connection |
| `remote-address` | string | the remote end's numeric address |
| `remote-port` | integer, 0 to 65535 | the remote end's port |

and may carry:

| Name | Type | Meaning |
| --- | --- | --- |
| `remote-host` | string | the remote end's name: for a client, from a reverse lookup the edge chose to do; for an outbound session, the host the server asked for |

Any other field is passed on as it is; an edge may describe anything else it
knows, such as a TLS server name or an SSH user. The name `direction` belongs
to the server. A control attachment carries no fields. A server refuses a
`HELLO` whose fields are malformed or don't meet these rules.

The server makes two things of the fields:

- `connection_name()` reads as it would on the TCP backend, so database code
  that parses it sees no difference behind an edge:
  `port LOCAL from HOST, port REMOTE` for a client and
  `port LOCAL to HOST, port REMOTE` for an outbound session, where HOST is
  `remote-host` if present and `remote-address` otherwise.
- `connection_info()` returns `{"direction", "inbound"}` or
  `{"direction", "outbound"}` followed by every field as a `{name, value}`
  pair, in the order the edge sent them. The TCP backend returns the same
  shape from its own sockets, so `connection_info()` reads the same with or
  without an edge.

The fields are fixed for the session: a resumed `HELLO` carries them again,
and the server takes the new copy.

Name lookups belong to the edge. A session server does no DNS of its own: the
edge resolves the host in a `CONNECT`, and an edge that does reverse lookups
for clients finishes them, within a bounded time, before it sends the `HELLO`.

The kind constrains the other fields:

- **client**: request is 0.
- **outbound**: listen port is 0. In initial mode the request names the
  `CONNECT` being answered and there is no binding; in graceful and crash mode
  the request is 0.
- **control**: initial mode, positions 0, no binding, listen port and request
  0. Only one control attachment is accepted at a time.

A server refuses a `HELLO` whose fields don't fit its kind.

Recovery mode tells the server why this attachment exists:

- **initial**: a new session. Positions are 0, there is no binding, and the
  database is zeros.
- **graceful**: the previous server detached the session cleanly with
  `DETACH`, at the positions given.
- **crash**: the previous server detached the session with a crash-mode
  `DETACH` after a panic, or the previous attachment ended without a valid
  `DETACH`.

The database field is the digest a valid `DETACH` gave the session since its
last `WELCOME`, and zeros if none did. A bound session is resumed only when it
names the database the server loaded ([Resuming a session](#resuming-a-session)).

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
| 0 | 1 | mode: 2 graceful, 3 crash |
| 1 | 7 | reserved |
| 8 | 8 | next input position |
| 16 | 8 | next output position |
| 24 | 16 | database: MD5 digest of the database file just written |

Sent by the server after it has written its shutdown dump (graceful) or its
PANIC dump (crash), and flushed all output for the session. The digest
identifies that file exactly; it is not a security measure. The server then
shuts down its side of the carrier. A server that couldn't write a dump sends
no `DETACH`.

The edge accepts a graceful detach only if both positions match its own, and
a crash detach only if the output position matches its own and the input
position is at most its own (input the server never read is lost, as in any
crash). It then attaches to the next server in that mode, at its own
positions, naming the digest. Any other detach is treated as a crash without
one.

### LISTENERS

Sent by the server on the control attachment, right after `WELCOME` and
whenever the set changes. The payload is the complete set of TCP ports the
server listens on through the edge, at most 1024 of them, each as one entry:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | port, 1 to 65535 |
| 2 | 2 | flags: 1 TLS, 2 ACME; other bits 0 |
| 4 | 2 | certificate path length |
| 6 | 2 | key path length |
| 8 | 2 | ACME names length |
| 10 | n | certificate path, UTF-8 |
| 10+n | m | key path, UTF-8 |
| 10+n+m | a | ACME names, comma-separated |

The paths are both empty or both non-empty, and only with the TLS flag. The
ACME flag needs the TLS flag, non-empty names and empty paths; the names are
empty without it. A TLS port with neither paths nor ACME uses the edge's own
certificate. A port appears at most once. The edge's
own port is not included; a `-t` port is, with the TLS flag and no paths.

### CONNECT

Sent by the server on the control attachment to ask for an outbound
connection.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | request ID, not 0 |
| 4 | 2 | port |
| 6 | 2 | flags: 1 TLS; other bits 0 |
| 8 | 4 | timeout in seconds, 0 for the edge's default |
| 12 | 2 | host length |
| 14 | 2 | reserved |
| 16 | host length | host name or address, UTF-8 |

### CONNECT_FAILED

Sent by the edge on the control attachment when it cannot make a requested
connection.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | request ID |
| 4 | 1 | reason: 1 the destination (lookup, refusal, timeout, TLS), 2 the edge's resources |
| 5 | 3 | reserved |
| 8 | rest | UTF-8 diagnostic, which the server logs |

### LISTEN_FAILED

Sent by the edge on the control attachment when it can't serve a port the
latest `LISTENERS` names.

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 2 | port |
| 2 | 1 | reason: 1 the port can't be opened, 2 there's no certificate to accept TLS with |
| 3 | 5 | reserved |
| 8 | rest | UTF-8 diagnostic, which the server logs |

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
   mode, with positions 0, no binding, and the connection fields.
3. The server sends `WELCOME` and treats the session as a new connection: it
   prints the welcome message and waits for a login command, exactly as for a
   new TCP connection.
4. When a login succeeds, the server sends `BIND`.

### Listening ports

A server builds its set of ports from `listen()` calls; it doesn't persist
them, so a database that wants a port opens it again in `#0:server_started`,
as with the TCP backend. Whenever an edge's control attachment begins, and
whenever the set changes afterwards, the server sends `LISTENERS`. The edge
listens on every port named and stops listening on any other it had opened for
a server. A port whose flags or paths change is opened again with the new ones.
Sessions already accepted on a port the edge stops listening on carry on.

On a TLS port the edge completes the TLS handshake before it attaches the
session, and describes it with `transport` `tls`. The server sees only the
plain bytes.

While no server is attached, the edge keeps listening on the ports the last
server asked for, and holds clients that arrive as it holds any unattached
session. A client's `HELLO` carries the port it arrived on. The server refuses
a new session for a port it isn't listening on; a resumed session is checked
against its binding instead, so it survives the port going away.

If the edge cannot listen on a port, cannot load or get a TLS port's
certificate, or has none of its own for a TLS port that names none, it sends
`LISTEN_FAILED`. The server logs it and drops the listener, as if `listen()`
had failed, so `listeners()` no longer shows the port and the next `LISTENERS`
leaves it out; the database can try `listen()` again. A failure on the `-t`
port is only logged, since no `listen()` call owns it. A certificate an edge
gets lazily, as ACME clients often do on a client's first handshake, fails
per handshake instead, and the edge only logs that.

### Outbound connections

1. A task calls `open_network_connection()`. The server allocates a request ID,
   suspends the task, and sends `CONNECT` on the control attachment. If there
   is no control attachment, the request waits for one.
2. The edge connects, with TLS if asked. If it fails, it sends
   `CONNECT_FAILED`, and the server resumes the task with the error.
3. If it succeeds, the edge opens an attachment for the new connection: `HELLO`
   in initial mode, kind outbound, carrying the request ID and connection
   fields with `transport` `tcp` or `tls` and `remote-host` set to the host
   the server asked for.
4. The server accepts it as an outbound connection, sends `WELCOME`, and
   resumes the task with the new connection object.

The server gives up on a request it hasn't heard back about within its timeout
plus ten seconds, resuming the task with `E_INVARG`, and it fails every
request it has sent when the control attachment ends. It refuses an outbound
`HELLO` for a request it is no longer waiting for, and if the task that asked
has gone by the time the connection arrives, it accepts the attachment and
immediately sends `CLOSE`.

From then on an outbound session is like any other. If the database logs it
in as a player, the server sends `BIND`, and the session is resumed when the
server is replaced. One that isn't logged in is refused by the replacement,
and the edge then closes the remote connection.

### Input

The edge reads from the client and sends `INPUT`. The server processes each
frame as it would bytes read from a TCP socket. In line mode it splits on CR,
LF or CRLF, drops unprintable characters, and carries a UTF-8 sequence split
across frames over to the next frame. In binary mode each frame's data is
delivered to the database as one binary string. After processing a frame, the
server acknowledges it.

Flow control is the carrier's own: there is no credit or window frame. The
server reads a session's carrier only while that connection's input isn't
suspended, and it suspends input when more than `MAX_QUEUED_INPUT` bytes are
waiting for the database (or the database holds input with `hold-input`).
The carrier's socket buffer then fills, and the edge's writes to it stop. An
edge must then stop reading that client too, so the client's own TCP window
closes; it may not keep reading and buffer the input instead. The server
acknowledges input as it reads it, so an edge also sees the pause as
acknowledgements that stop advancing.

### Output

The server sends `OUTPUT` frames, splitting at 1 MiB. It queues at most
`MAX_QUEUED_OUTPUT` bytes of output for a session, as the TCP backend does.
When new output wouldn't fit and the carrier can't take more, output the
caller doesn't allow to be flushed (`notify()` with no-flush) is refused, and
otherwise the oldest queued output that hasn't started going to the edge is
dropped. The next output the edge receives is then the line
`>> Network buffer overflow: N lines of output to you have been lost <<`,
where the dropped lines would have been. A frame takes its output position
when the server starts writing it, so dropping queued output leaves no gap in
positions.

The edge writes output to the client and acknowledges it. When a client
doesn't read, the edge stops reading that session's carrier rather than
buffer without bound, which is what makes the server's queue fill.

### Ending a session

Either side sends `CLOSE`, as described above. If the carrier ends without
`CLOSE` while the server is running, the server treats the session as ended:
an edge that loses its carrier and still has the client attaches again, while
an edge that has died takes its sessions with it.

### Graceful replacement

A server shutting down in an orderly way (the `shutdown()` builtin, `SIGINT`,
`SIGTERM` or `SIGUSR1`) sends every player the usual `*** Shutting down ***` line and
writes its final database, which records every logged-in player and listener
pair. Only then does it send each session `DETACH` with its current positions
and the digest of that database, and close the carrier. The server writes out
every session's queued output and `DETACH` together and waits for the edge to
close each carrier, with one deadline of five seconds for all of them, so a
server with many sessions, or a stalled edge, still stops promptly. A session
that misses the deadline is logged; its edge sees the carrier end and treats
it as a crash. Each edge keeps its clients
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
   mode with the positions it holds, the binding, if any, and no database.

With no database named, the replacement resumes no logged-in player: whatever
database it loaded, a checkpoint included, may disagree with the sessions
about who is connected. Their carriers are refused, and the edge closes those
clients. Sessions that weren't logged in carry on as new connections.

### Resuming a session

When a `HELLO` carries a binding (graceful or crash mode), the server resumes
the session only if:

- its database field is the MD5 digest of the database file the server
  loaded, so that file is exactly the dump written as the session was
  detached, and
- the player is a valid object that isn't already connected, and
- the player and listener pair is in the list of formerly active connections
  recorded in the database the server loaded, and
- the listener still exists.

If those hold, the server takes that pair out of the list, creates the
connection already logged in as the player, logs `SESSION REATTACHED` or
`SESSION RECOVERED`, and sends `WELCOME`. It does not call
`$do_login_command`, `user_connected` or `user_reconnected`, and it doesn't
send a welcome message. After a panic it sends the player the line
`*** Session recovered from a server crash ***`. If any check fails, the
server closes the carrier without `WELCOME`.

The digest check is what makes resuming safe: only the dump the old server
wrote at the moment it detached the session records the connection as it
stood. Any other database, older or newer, could name the player as connected
when they weren't, or the reverse, and could hold different state for them.
After resuming, a session names no database until its next `DETACH`, so a dump
resumes each session at most once.

A `HELLO` in initial mode that carries a binding is refused. A `HELLO` in
graceful or crash mode without a binding is accepted as a new, not yet logged in
connection.

At startup, the TCP backend tells the database that every connection recorded
in the loaded database has disconnected. The session backend waits instead,
since those players are expected to resume: for `$server_options.session_resume_timeout`
seconds from startup (60 if unset), and then calls
`#0:user_disconnected` for every recorded connection no edge has resumed,
logging `SESSION NOT RESUMED`. A session that arrives after that is refused
like any other that the database doesn't record.

### Panics

When the server panics, it writes a PANIC database (`output.db.PANIC`), which
includes the formerly active connections. If that succeeds, it sends each
session a crash-mode `DETACH` naming the PANIC database, without a shutdown
message, and exits. Restarting the server from the PANIC database resumes the
sessions in crash mode with all state up to the panic, except the task that
was running. Restarting it from anything else, such as the last checkpoint,
resumes nobody, as after a crash without `DETACH`.

## Edge obligations

An edge implementing this protocol must:

- choose session IDs that are random and unique across every edge, and a fresh
  edge instance ID for each process;
- increase a session's generation on every new attachment;
- send `HELLO` first and wait for `WELCOME` before sending input;
- name in `HELLO` the database from a `DETACH` it accepted since the session's
  last `WELCOME`, and otherwise zeros;
- keep its input and output positions, and the binding from the most recent
  `BIND`;
- stop reading client input while a session is unattached;
- never replay input after a crash;
- send `CLOSE` when the client goes away, and close the client on `CLOSE`;
- keep a control attachment open while it can reach a server, listen on the
  ports the latest `LISTENERS` names, keep listening on them while no server
  is attached, answer every `CONNECT` with a connection or with
  `CONNECT_FAILED`, and send `LISTEN_FAILED` for a port it can't serve;
- verify the certificate of a TLS outbound connection against the host name
  in `CONNECT`;
- terminate TLS on ports `LISTENERS` flags as TLS, with the paths it names, a
  certificate it gets and renews by ACME for the names given, or the edge's
  own certificate;
- bound every buffer: hold at most one read (64 KiB is a good size) of a
  client's input that hasn't been written to the carrier, and stop reading the
  client while the carrier won't take more; hold at most a bounded amount of a
  session's output (1 MiB is a good size) that hasn't been written to the
  client, and stop reading the carrier while it is full. An edge never drops
  input, and drops output only by closing the session.

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
  It carries client sessions on its own port only, without a control
  attachment.
- The lambdamoo-tests suite contains a Python implementation of the edge side,
  including the control attachment, listening ports, and TLS both ways, used
  to test the server.
