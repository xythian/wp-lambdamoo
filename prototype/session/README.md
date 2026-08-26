# Connection-independent session prototype

This directory implements the first carrier described in
[`Connection_Session_Protocol.md`](../../Connection_Session_Protocol.md): one
secured Unix-domain stream per session attachment, with a stable TLS edge
retaining the external connection while backend processes are replaced.

The prototype includes both a standalone echo/large-output harness and the
configure-selectable LambdaMOO `session` networking backend. The latter has
preserved one TLS connection across a forced server crash and checkpoint resume.

## Components

- `protocol.c` and `protocol.h`: bounded framing and control codecs shared by
  both C processes.
- `edge.c`: threaded C reference edge. It terminates TLS, owns globally unique
  session IDs, authenticates the local backend with peer credentials, and
  reconnects each session independently.
- `backend.c`: C harness which validates attachment generations and byte
  positions, acknowledges input ownership, emits positioned output, and can
  request graceful replacement.
- `test_protocol.c`: codec, framing, truncation, and malformed-header tests.
- `integration_test.c`: process-level TLS and replacement scenarios.
- `PROTOCOL.md`: the prototype 1.0 wire contract.
- `../../net_session.c`: LambdaMOO endpoint, line/binary adaptation, bounded
  output, and checkpoint-authorized session restoration.

## Build and test

A C11 compiler, POSIX threads, OpenSSL 3 development headers and libraries, and
the OpenSSL command-line tool are required.

```sh
make -C prototype/session test
make -C prototype/session test-server-integration # after a session-backend MOO build
make -C prototype/session benchmark
make -C prototype/session test-sanitize
```

The integration test creates a one-day self-signed certificate with a
`localhost` subject alternative name. The test client trusts that certificate
and performs hostname verification; TLS is not merely encrypted without
authentication.

Set `CC`, `CFLAGS`, or `OPENSSL` in the usual Make manner when needed. The
prototype has no installation target and does not affect the normal server
build.

Build the server endpoint with:

```sh
./configure --enable-net=session,poll,require
make
./moo -l moo.log input.db output.db session.sock
```

Point the reference edge at that Unix socket. On first attachment MOO sends a
`BIND` after login. Following a checkpoint and backend loss, the edge reconnects
with the player/listener binding; MOO accepts it only when the pair occurs in
the loaded checkpoint. The ordinary `tcp` networking backend remains the
default.

The full-server integration test preserves one TLS object through a forced
crash and a signal-driven graceful shutdown, checks the corresponding recovery
mode in MOO logs, and submits commands after each replacement. The benchmark
retains 100 TLS sessions and reports small-message latency percentiles plus a
4 MiB transfer rate; see [`EVALUATION.md`](EVALUATION.md).

## Demonstrated semantics

The edge owns external TCP/TLS and stops reading it while no backend attachment
exists. Input successfully written to a backend advances the no-replay
position. If that write fails, the just-read edge buffer is discarded. A new
backend attaches at the next byte positions using a higher generation.

A valid graceful detach preserves the stated positions and labels the next
attachment graceful. An EOF, protocol failure, or process death labels it a
crash. The backend harness turns those modes into visible attachment banners so
the integration test can verify the distinction through the unchanged TLS
connection.

Unix socket mode `0600` and same-effective-UID peer credential checks secure
the initial local carrier. The random edge and session IDs are identities
inside that authenticated boundary, not bearer credentials.

## Prototype limitations

This code favors a legible experiment over a production event loop:

- one thread and one Unix stream are used per external session;
- TLS and frame reads are blocking within that session's worker;
- accepted output is copied through allocated frame buffers;
- gateway configuration, certificate rotation, logging, and metrics are absent;
- backend discovery is a fixed Unix socket path with retry;
- sessions survive backend failure but not failure of their owning edge;
- origin metadata is carried but has no structured schema yet; and
- the standalone harness does not model MOO login/checkpoint authorization.

These limitations are interface work or alternative-carrier work. None requires
multiplexing to change the session-visible contract.
