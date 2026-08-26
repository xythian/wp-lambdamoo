# Prototype evaluation

Date: 2026-08-26.

## Result

The one-stream-per-session design is viable at the prototype level. It
demonstrates the central architectural property without a multiplexing library:
a certificate-validated TLS client remains on the same external connection
while its session moves between C backend processes.

The automated optimized suite completed successfully on the development host.
A clean build, certificate generation, unit tests, and all integration
scenarios took 1.10 seconds wall-clock. This timing is only a test-cycle
guardrail; it is not a throughput benchmark.

The same unit and integration scenarios also passed with AddressSanitizer and
UndefinedBehaviorSanitizer enabled.

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
| Sanitizers | Complete process topology runs under ASan and UBSan | Pass |

## What the experiment says

A broker is not required merely to support several edge implementations.
Independent TCP/TLS and WebSocket edges can each own their external connections
and open per-session streams to the same backend endpoint. A rendezvous service
may still be useful for finding a replacement backend, but it does not need to
carry session data.

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

## Remaining questions

The harness does not prove that the existing LambdaMOO server can restore an
in-database connection without invoking its normal disconnect path. The next
implementation step is a selectable `network.h` backend, including line and
binary input, output queue limits, suspension, echo options, trusted origin
metadata, and the server/database resume boundary.

The prototype also does not yet answer:

- what in-database state identifies a resumable connection after checkpoint
  rollback;
- whether crash recovery resumes a player directly or calls a database hook;
- how user-visible recovery events are represented outside the harness;
- how a replacement endpoint is published to several edges in production;
- whether partial TLS records waiting in the external kernel socket should be
  treated as post-recovery input; or
- whether Waterpoint needs edge-process failover in addition to backend
  failover.

Those decisions should be made while integrating the real server rather than by
adding machinery to the carrier.
