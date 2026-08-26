# Prototype evaluation

Date: 2026-08-26.

## Result

The one-stream-per-session design is viable end to end. A certificate-validated
TLS client remains on the same external connection while its session moves
between both harness backends and real LambdaMOO processes. The selectable MOO
backend authorizes player restoration against the loaded checkpoint and handles
both crash and graceful replacement.

The automated suite covers codec behavior, synthetic-backend replacement, and
real-MOO crash and graceful replacement. The protocol codec suite also passes
with AddressSanitizer and UndefinedBehaviorSanitizer enabled.

## Scenario evidence

| Scenario | Evidence | Result |
| --- | --- | --- |
| Ordinary attachment | TLS client receives an initial attachment event and echoed input | Pass |
| Backend crash | Backend A is killed with `SIGKILL`; backend B attaches at retained byte positions | Pass |
| TLS continuity | The pre-crash `SSL *` exchanges input and output after backend B starts | Pass |
| No input replay | The edge advances input positions only after giving bytes to the backend and never retains a replay queue | Pass by implementation and position checks |
| Graceful replacement | Backend sends a graceful detach with both positions, exits, and backend C resumes in graceful mode | Pass |
| Recovery signaling | Client-visible harness events distinguish initial, crash, and graceful attachment | Pass |
| Large output | A 3 MiB response crosses multiple bounded protocol frames intact | Pass |
| Slow-session isolation | One session requests 8 MiB and stops reading while a second session attaches and echoes data | Pass |
| Multiple edges | Two C edge processes attach independent TLS sessions to one backend socket | Pass |
| Malformed private peer | A same-UID peer sends an invalid header; its attachment is closed while existing sessions continue | Pass |
| Transport authentication | Unix peers must have the same effective UID; the TLS client validates the generated certificate and `localhost` name | Pass |
| Codec safety | Truncation, invalid magic, fixed-size control codecs, byte order, and size bounds are unit-tested | Pass |
| Sanitizers | Protocol codec suite runs under ASan and UBSan | Pass |
| Real MOO crash | Minimal.db connection #3 survives `SIGKILL`, checkpoint load, and accepts a later command | Pass |
| Real MOO graceful restart | Signal shutdown drains and detaches; replacement resumes without crash mode | Pass |

## Basic workload benchmark

`make -C prototype/session benchmark` measures the reference TLS edge and harness
on loopback. It retains 100 sessions, measures 500 sequential 32-byte echo round
trips on one established session, and transfers five 4 MiB responses. This models
the expected hundreds-of-connections scale, perceived command lag, and large
Waterpoint extraction-style output. It is not a saturation benchmark.

On the 2026-08-26 development host (Linux 6.8, AMD Ryzen 9 9955HX), one run
produced:

| Measurement | Result |
| --- | ---: |
| 100 sequential verified-TLS attachments | 4,281.742 ms total; 42.817 ms mean |
| Established-session RTT p50 / p95 / p99 | 0.013 / 0.025 / 0.080 ms |
| Five 4 MiB responses | 1.495 ms median; 1.602 ms maximum; 2,676.202 MiB/s median |

The loopback throughput number is primarily a sanity ceiling and will not
predict a real client path. The useful conclusion is that this prototype adds
no locally measurable lag at MOO scale and handles multi-megabyte output with
large headroom. TLS setup, operational behavior, copying, and failure semantics
remain more important selection criteria than raw carrier speed.

## What the experiment says

A broker is not required merely to support several edge implementations.
Independent TCP/TLS and WebSocket edges can each own their external connections
and open per-session streams to the same backend endpoint. A rendezvous service
may still be useful for finding a replacement backend, but it does not need to
carry session data.

Harbor already supplies the broker layer above this boundary. Its `runtimeSession`
owns one persistent MOO upstream and accepts multiple frontend attachments. A
`connect` attachment mirrors session output, while `terminal` runs one scoped
command by sending unique `PREFIX` and `SUFFIX` markers to MOO and routing the
lines between them only to the requesting terminal. Harbor can therefore replace
its ordinary upstream dial/redial loop with a Go implementation of this session
carrier without moving multi-client semantics into the MOO protocol.

That would make MOO replacement invisible to Harbor frontend attachments and
avoid reconnecting and logging the player in again. It would not eliminate the
terminal marker protocol: the carrier transports an ordered byte stream and does
not identify which output belongs to a command. Recovery mode should be exposed
to Harbor as an out-of-band event. Harbor can preserve an active terminal command
across a coordinated graceful replacement, but after crash recovery it must fail
the command explicitly because the checkpoint may predate its task or suffix. A
client-visible recovery line is insufficient and could otherwise be mistaken for
command output.

At the anticipated scale, one backend descriptor and worker context per session
is a reasonable first implementation. It removes outer stream IDs,
cross-session scheduling, and multiplexed flow-control state. The slow-session
scenario confirms that the isolation comes naturally when sessions do not
share a carrier connection.

Byte positions and attachment generations are sufficient to fence a stale
backend and preserve ordering. They do not make a server crash transparent:
input already given to a failed backend is not replayed, state may roll back to
an older checkpoint, and the next attachment explicitly reports a crash.

The current copying behavior is acceptable for validating the boundary but is
not optimized. External input is decrypted into an edge buffer, copied into a
framed payload, copied through the Unix stream, and allocated by the backend
reader. Output follows the reverse path. `writev`, retained buffers, or a
shared-memory data carrier could reduce these copies without changing session
identity, positions, acknowledgements, or recovery modes.

## Remaining work

The server integration answers the original checkpoint boundary: a resumed
player/listener pair must occur in the loaded list of formerly active
connections, and the edge carries that binding without treating it as a bearer
credential. Crash recovery resumes the player directly and emits an explicit
client notice; graceful recovery uses a bounded detach drain.

Production work still includes:

- a non-thread-per-session reference edge event loop;
- structured trusted-origin metadata and connection-option policy;
- certificate rotation, metrics, configuration, and endpoint discovery;
- coordinated graceful quiescing across many sessions rather than bounded
  per-session shutdown waits;
- a decision about partial TLS records waiting in the external kernel socket;
- load tests on a realistic Waterpoint database and host; and
- edge-process failover, if Waterpoint requires it in addition to backend
  failover.
