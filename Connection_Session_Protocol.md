# Connection-independent session protocol

Status: working end-to-end prototype, 2026-08-26.

The C reference edge, backend harness, protocol implementation, tests, and
scenario results are in [`prototype/session/`](prototype/session/README.md).
The `session` configure option selects a LambdaMOO `network.h` backend using the
same protocol. The complete path has retained a verified TLS connection across
a forced server crash, restored its player/listener binding from a checkpoint,
and processed subsequent input in the replacement process. Graceful quiescing,
operational hardening, and an automated full-server scenario remain future
work.

## Objective

Allow a user-visible MOO session to survive replacement or failure of the MOO
server process without requiring the client to reconnect.

The central design decision is to decouple a session from every particular
network connection used to carry it. A stable edge owns the external client
connection. A replaceable MOO server attaches to the corresponding session
through a private protocol. If that attachment is lost, the edge retains the
external connection and permits a new server process to attach safely.

The first protocol should be deliberately small. It may use one private stream
per session and does not need multiplexing. It may rely on an authenticated and
confidential carrier rather than defining encryption itself.

## Model

Three lifetimes must not be conflated.

### External connection

An external connection is the transport between a client and an edge. It can be
TCP, TLS, WebSocket, HTTP, or another gateway-specific protocol. The edge owns
its transport state, including TLS keys, partial frames, and peer information.

An external connection may end while a durable application session remains, or
it may remain open while one or more backend attachments are replaced.

### Session

A session is the stable, user-visible conversation represented to the MOO
server. It has an opaque, unguessable identity and trusted origin metadata. Its
lifetime is independent of a particular edge-to-server connection and a
particular MOO process.

The session corresponds to what the server currently represents in-database as
a connection. The protocol separates that object from the external transport
and from the current server process without requiring a new user-visible
concept.

A session records enough attachment state to establish a clean boundary when a
backend is replaced. It does not imply that an executing MOO task,
uncheckpointed database mutation, or arbitrary process memory survives a
crash. In particular, a replacement server may have loaded an older checkpoint
which does not contain tasks that were blocked in `read()`.

The edge is initially authoritative for the existence of live sessions because
it owns the external connections. Which additional session state is durable,
and where it is stored, remains an explicit policy decision.

### Multiple edges

The architecture must permit several edge processes at once. For example, a C
reference edge may own conventional TCP and TLS listeners while a separate web
server owns WebSocket connections. Each edge remains authoritative for the
sessions whose external connections it owns, and each independently attaches
those sessions to the same backend service.

Multiple edges do not inherently require a broker. With the initial
one-stream-per-session carrier, the backend can accept authenticated attachment
connections from any permitted edge. Session identifiers must be globally
unique rather than merely unique within an edge, and attachment setup must
include the edge instance identity and generation. After backend replacement,
each edge discovers the replacement and reattaches its own live sessions.

Some small rendezvous or control-plane facility may still be useful for backend
readiness, endpoint discovery, authorization, and coordinated shutdown. It can
be built into the reference edge, supplied by a supervisor, or introduced as a
separate bespoke service. It should not carry session data or become the owner
of sessions unless a demonstrated requirement calls for that role.

A broker or shared session owner becomes necessary if a session must migrate
between edge processes or survive failure of the edge that owns its external
connection. Those are distinct availability goals and are out of scope for the
first prototype. The interface must not preclude adding such an owner later.

### Backend attachment

An attachment is a temporary association between one session and one MOO server
instance. Attachments have generations or leases so a stale backend cannot
continue reading or writing after ownership has moved.

Loss of an attachment is expected and does not itself close either the external
connection or the session. At most one attachment may be active for a session
at a time.

## Visible contract

The session layer should expose operations equivalent to:

- create or discover a session;
- attach a backend at an agreed generation and delivery position;
- carry ordered client input and server output;
- acknowledge precisely defined acceptance points;
- suspend either direction under bounded flow control;
- detach without closing the session;
- resume through a replacement attachment; and
- close or abort the session with an explicit reason.

Names and binary encodings are intentionally not frozen yet. The contract and
state machine should be specified before the wire representation.

A carrier may introduce its own connection identifiers or stream identifiers,
but those are never session identities and cannot appear in the user-visible
contract.

## Identity and ownership

Each session needs a stable opaque identifier with sufficient entropy to resist
guessing. Possession of an identifier alone should not authorize attachment.
The secured carrier authenticates the edge and backend, and the protocol checks
that the peer is permitted to act for the session.

Each edge and backend process also has a fresh instance identity. A monotonically
changing attachment generation, combined with those identities, fences delayed
messages and stale processes. The edge must reject output from any attachment
that no longer owns the current generation.

The initial implementation should make the edge the attachment coordinator.
More elaborate distributed ownership is out of scope.

## Delivery positions

Each direction is an ordered sequence of opaque byte spans. Sequence positions
refer to byte boundaries, not protocol-frame counts, so changing frame sizes or
carriers does not change the session contract.

Acknowledgements exist to bound buffers and transfer ownership between the
edge and backend. The edge does not need to know whether input created a MOO
task or whether that task completed. Once the edge gives input to the backend,
it will not replay that input to another attachment.

A graceful replacement may quiesce both directions and agree on a clean byte
boundary before changing attachments. A backend crash has deliberately weaker
semantics:

- input already given to the failed backend is never replayed;
- input buffered at the edge for that attachment is discarded;
- the edge stops reading additional client input until recovery policy permits
  it to resume; and
- the replacement may restore the session from an older checkpoint, including
  one without tasks that had been waiting in `read()`.

The protocol must distinguish graceful replacement from crash recovery. The
edge or a higher-level gateway can translate that event into appropriate user
messaging, suppress it where policy permits, or expose it to in-database resume
logic. Preserving the external connection after a crash is useful even though
the logical interruption is not transparent.

Output already accepted from the failed backend may be drained to the client.
The protocol does not claim that the client observed it, or that output and
database state are transactionally consistent across a crash.

## Flow control and buffering

Every buffer must have a configured bound. A slow client must not permit
unbounded server output, and an absent backend must not permit unbounded client
input.

Flow control belongs to the session contract even when a carrier supplies its
own backpressure. Carrier-level writability only says that the carrier can
accept bytes; it does not establish that the session peer has accepted them.

The edge should normally pause reading from the external connection when its
input allowance is exhausted. Gateway protocols that cannot exert useful
backpressure must define an explicit overflow policy.

Large transfers must remain opaque data rather than being serialized into a
general object graph. This permits vectored writes, reference-counted buffers,
and future shared-memory transport without changing session semantics.

## Initial carrier boundary

The first carrier may use one secured byte stream per attached session. At the
expected scale of hundreds of sessions, this is an acceptable and valuable
simplification.

A per-session stream avoids:

- an outer logical stream identifier;
- cross-session scheduling in the protocol;
- shared-connection head-of-line behavior;
- multiplexed flow-control accounting; and
- recovery of an entire bundle when one carrier connection fails.

A small control connection may be added only for operations that are genuinely
not per-session, such as backend registration, readiness, session enumeration,
or coordinated shutdown. It must not become an accidental second source of
session truth.

For a same-host reference implementation, Unix-domain sockets secured by
filesystem permissions and verified peer credentials are the preferred
carrier. Remote operation may use mutually authenticated TLS or another
transport providing equivalent peer authentication, confidentiality, and
integrity.

The session protocol does not define TLS, certificate issuance, authorization
policy, or carrier reconnection timing. It does define what an authenticated
peer may do after a new carrier connection is established.

## Reference implementation

Build both the reference edge and the MOO endpoint in C. This provides a small
normative implementation, exercises the same event-loop and memory-management
constraints as the server, and avoids making a Go runtime or a particular RPC
framework part of the protocol definition.

The reference edge should terminate TLS for ordinary clients so the prototype
demonstrates that TLS state remains live across a backend crash. Its TLS
configuration should be suitable for testing and reference deployment, while
certificate provisioning remains an operator concern.

Waterpoint may implement its production edge in Go against the same documented
boundary. Harbor, the WebSocket gateway, and future frontends can either speak
the session protocol directly or sit above another gateway. Extra copies in a
general gateway are an acceptable initial price for clean indirection; optimized
carriers can be introduced where measurements justify them.

## Wire-format constraints

The first encoding should be custom and small:

- fixed byte order and integer widths;
- incremental parsing over arbitrary read boundaries;
- hard limits before allocation;
- version and capability negotiation;
- opaque bulk-data fields;
- length-delimited extensible metadata;
- no dependence on C structure layout; and
- deterministic rejection of unknown required operations.

Protobuf, Cap'n Proto, gRPC, ZeroMQ, and similar systems are not required for
the initial protocol. They remain possible gateway or carrier implementation
choices, but must preserve this session contract at the boundary.

The wire protocol need not carry a session identifier on every data record
when the secured stream has already been bound to exactly one session. It must
still make attachment identity and generation unambiguous during setup and
failure handling.

## Recovery outline

A replacement sequence should have the following shape:

1. The edge detects loss of the current backend attachment.
2. It fences that attachment generation and stops accepting its output.
3. It applies bounded backpressure to client input while retaining the external
   connection.
4. A replacement backend authenticates and advertises readiness.
5. The peers identify the session, the crash or graceful-replacement mode, and
   the relevant session metadata.
6. For a graceful replacement they agree on delivery positions. After a crash,
   the edge reports the discontinuity and does not replay old input.
7. The edge grants a new attachment generation.
8. Ordered traffic resumes without changing the external connection.

A planned upgrade can quiesce at cleaner acknowledgement boundaries, but it
must use the same attachment rules as crash recovery. Graceful handoff is an
optimization of the failure-safe mechanism, not a separate correctness model.

## Future carriers

Multiplexing can later carry many attachments over one connection by adding an
outer mapping from carrier stream IDs to session attachments. The mapping must
not alter session identity, ordering, acknowledgement, or recovery semantics.

A local zero-copy carrier may use shared-memory rings for bulk data and a Unix
socket, pipe, or event descriptor for notification. Buffer descriptors would
replace inline data without changing sequence positions. Such a carrier must
define region ownership, reclamation after peer death, generation fencing, and
how a slow session avoids pinning shared capacity.

Descriptor passing may also optimize particular gateway arrangements. Passing
a descriptor changes the data path and ownership implementation, but must not
silently weaken the session contract.

## Prototype milestones

1. Specify the session and attachment state machines, including every failure
   transition and acknowledgement meaning.
2. Define the minimal per-session handshake and framed byte-stream encoding.
3. Implement a C reference edge and a small C backend harness over Unix-domain
   sockets.
4. Demonstrate ordered bidirectional traffic, bounded backpressure, malformed
   input rejection, backend death, replacement, and resumed traffic through
   one unchanged TLS client connection.
5. Integrate the backend side with the server's existing network abstraction.
6. Add a second independent implementation, likely the Waterpoint Go edge, to
   validate that the written protocol rather than shared C code defines the
   boundary.

The prototype should measure perceived pause time, memory retained per idle and
blocked session, large-response behavior, and copy counts. Peak message
throughput is a guardrail, not the selection criterion.

## Open questions

- What session metadata must survive a server crash, and which component owns
  each field?
- What acknowledgement is sufficient to transfer buffer ownership without
  exposing MOO task lifecycle to the edge?
- What crash/recovery event should be represented to database code and users?
- Does resumption restore a logged-in player directly, or invoke an
  authenticated in-database resume hook?
- Can the normal network interface represent attachment loss without treating
  it as a permanent client disconnect?
- Is a separate control connection necessary, or can discovery and attachment
  remain completely per-session?
- Which discovery or rendezvous mechanism lets independent edge processes find
  and coordinate with a replacement backend?
- Must sessions ever migrate between edges or survive edge-process failure, and
  would that justify a shared broker or session owner?
- Which limits and timeout policies belong to the protocol, and which are local
  edge policy?
