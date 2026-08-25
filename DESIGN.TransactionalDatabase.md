# In-process checkpointing, transactional storage, and replay

This prospective document explores replacing the server's dependence on forked
checkpoints and, independently, replacing its monolithic in-memory database with
a transactional persistent backend.

LMDB, SQLite, or a similar transactional store is one candidate for the larger
backend experiment.
It is not required merely to checkpoint without fork. EtaMOO and rmoo are
relevant proof-of-concept directions for concurrency and transactional replay;
this document does not assume their exact implementation.

## Three separable designs

There are at least three checkpoint/storage configurations:

1. Forked textual checkpoint

   The current process forks and the child serializes its copied address space.
   The parent resumes quickly, but the child inherits foreign-library threads,
   mutexes, allocators, and runtime state which may not be safe after fork.

2. Unforked in-process textual checkpoint

   The current database format and encoder hooks remain. The server serializes
   in-process, either while task execution is quiesced or from a stable
   in-memory snapshot. This removes post-fork library hazards without requiring
   a new persistent backend. The existing UNFORKED_CHECKPOINTS option is a
   simple stop-the-world form, though a production design may need better pause
   and snapshot behavior.

3. Transactional/MVCC backend

   Committed state lives in LMDB or another transactional store. Checkpointing
   becomes publication, backup, or copying of a committed backend snapshot.
   This can also support concurrent readers, multithreaded task attempts,
   optimistic conflict detection, and replay.

These are incremental choices, not one indivisible migration. We can adopt the
provider contract required by unforked checkpoints before deciding on LMDB.

SQLite also supports two distinct experiments which cut across this list:

- use a SQLite database file only as the checkpoint format while canonical live
  state remains in memory; or
- make SQLite the live backing store from which tasks read and to which commits
  publish.

The first tests serialization, indexing, incremental checkpoint construction,
and reload behavior. The second changes the server's transaction and concurrency
model. Success with the first neither requires nor proves the second.

## Common persistence contract

All three designs benefit from one invariant:

> Persistent state is a pure projection of canonical logical state.
> Persistence must not depend on a live foreign stack or transient runtime
> handle.

A provider encoder must not advance computation. Under a forked checkpoint it
must not enter a foreign runtime or acquire inherited foreign mutexes. Under an
unforked checkpoint it must see state which cannot mutate concurrently beneath
it. Under an MVCC backend it may encode an immutable committed revision rather
than a live object.

Transient SQLite connections, WASM engines, JIT artifacts, parser stacks, and
worker objects are caches. They are never the only representation of persistent
state.

## Unforked checkpoints with the current backend

Moving to unforked checkpoints is possible without transactional storage, but
pure encoders alone are insufficient once the server has threads.

The checkpoint must observe a coherent database-wide state. Candidate models
are:

- Stop the world while the current textual dump is written.
- Stop briefly to create a stable COW/immutable snapshot, then serialize it
  concurrently.
- Hold a global snapshot epoch while writers replace versioned structures.
- Journal mutations after a checkpoint boundary while a snapshot is written.

The first is simplest and already conceptually supported. Its cost is checkpoint
latency. The others begin to introduce the same immutable/versioned state needed
by an MVCC backend.

A useful reference measurement is roughly two seconds for Waterpoint to write a
roughly 40 MB on-disk database on a Linode Nanode. This is not a LambdaMOO
measurement; LambdaMOO textual serialization may be slower. Two seconds is too
long for a routine stop-the-world pause, but the result suggests that faster
serialization could make synchronous checkpoints between task steps practical.
We should benchmark directly before assuming a concurrent snapshot or
new backend is required.

An unforked checkpoint occurs at a scheduling boundary: no MOO task is running
and no transaction is half-committed. Network input remains queued during the
pause. The acceptable pause budget depends on the deployment workload.

Candidate optimizations include profiling traversal, formatting, allocation,
copying, writes, flush, and fsync; reducing formatting and intermediate copies;
larger buffered writes; precomputed immutable provider encodings; incremental
dirty-record checkpoints with periodic full snapshots; and a binary or indexed
format if textual serialization dominates. Durability must not be reported
before required writes and publication complete.

A short synchronous pause is much simpler than concurrent dump serialization:
it needs no snapshot epoch, writer barrier, or provider concurrency and removes
post-fork hazards immediately. Transactional storage should be justified by
concurrency, replay, incremental persistence, or measured checkpoint limits,
not by assuming an unforked full dump must be too slow.

## SQLite storage experiments

### SQLite as a checkpoint format

The server retains its current in-memory object graph and writes a complete,
self-contained SQLite file at a scheduling boundary. Tables can represent
objects, properties, verbs, tasks, interning data, bound identities, provider
payloads, and format metadata. Values may initially use one canonical encoded
blob per field; normalizing every list element or value edge is a separate
experiment and should be justified by measured query or incremental-update
benefits.

The simplest safe implementation builds a new database beside the published
checkpoint, commits it, closes it, fsyncs the file and containing directory as
required, and atomically renames it into place. It must not update the only good
checkpoint in place. SQLite's journal/WAL files and durability settings are
part of the checkpoint publication protocol, not deployment details.

This experiment can answer:

- whether binary values, prepared inserts, batched transactions, and reduced
  formatting make a full stop-the-world checkpoint fast enough;
- whether indexed or selective loading materially improves restart time;
- how file size, temporary space, write amplification, and fsync time compare
  with the textual format; and
- whether a later incremental checkpoint can safely reuse unchanged records.

The output should remain a portable checkpoint, not an accidentally live
database. A schema version and deterministic logical encoding are required.
Tests must verify a cold reload into a fresh server, integrity checking, failed
publication recovery, and conversion back to the ordinary logical model.

### SQLite as the live backing store

In the larger experiment, committed objects and values live in SQLite and each
task attempt uses a database snapshot or an explicit read/write set. Commit
publishes the attempt atomically; a conflict causes replay or a defined failure.
This explores the same task-attempt model as the LMDB proposal, but SQLite has
different concurrency properties: WAL can permit concurrent readers while
writes are still serialized, and long-lived readers affect WAL checkpointing
and file growth.

The first prototype should favor a simple schema and correctness over exposing
SQL as the server's object model. It must measure statement and encoding
overhead for fine-grained MOO access, cache behavior, write amplification,
writer contention, transaction duration across task execution, and the cost of
materializing complex aliased values. Suspended tasks cannot retain an open SQL
transaction indefinitely; suspension remains a commit boundary.

The server's SQLite-backed bound type is logically separate from this backend.
A bound SQLite value is guest data with its own canonical image and permissions;
the server backend stores MOO state. Nested use must not create lock-ordering,
reentrancy, or transaction-coupling requirements between the two databases.

Both SQLite experiments use the same generated stress databases and correctness
oracle as the textual and LMDB experiments. Benchmark SQLite's journal modes and
durability levels explicitly, but compare only configurations with equivalent
crash guarantees.

### Checkpoint benchmark and stress databases

The test suite needs databases much larger and less friendly than its functional
fixtures. Checkpoint work should use generated, reproducible cases rather than
relying only on one production database.

The matrix varies on-disk and live size; object, property, verb, string, list,
WAIF, and bound counts; a few huge values versus many tiny values; deep and wide
containers; aliasing, chains, cycles, and unreachable graphs; large byte blobs,
many small blobs, streams, struct layouts, and bound sidecars; suspended tasks
with large shared environments; string-interning hit rates; provider encoding
expansion; dirty-record ratios; storage throughput; and explicit fsync cost.

Pathological cases exercise maximum legal nesting and counts, adversarial alias
graphs, huge properties, unavailable providers, decode failures, opaque values,
and escaping- or formatting-heavy data. Limits must make these slow or reject
them predictably, never overflow or allocate unbounded temporary storage.

Measurements separate task-pause time, CPU, bytes generated, temporary memory,
write time, flush and fsync, final publication, and reload time. Forked and
unforked modes use the same fixtures. Binary, incremental, snapshot, and MVCC
prototypes report against the same matrix so comparisons remain meaningful.

Provider rules for an unforked dump are:

- encode committed canonical state only;
- never mutate while encoding;
- use immutable state, a provider snapshot, or core-managed locking;
- never retain an unbounded lock across database output;
- do not call MOO, workers, or external services;
- keep aliases and graph references stable for the snapshot epoch.

A future checkpoint API may distinguish:

    prepare snapshot       runs in the live server at a safe point
    encode snapshot        pure, possibly concurrent
    release snapshot       runs after success or failure

The prepared representation must be ordinary immutable provider state, not a
foreign handle whose library must remain callable during serialization.

## Transaction and replay model

The larger concurrency experiment runs a MOO task as transaction attempts:

    begin attempt
      open consistent read snapshot
      execute MOO
      collect database/provider mutations
      collect deferred effects
    commit
      no conflict -> publish, then deliver effects
      conflict    -> discard attempt and replay

LMDB naturally offers many readers and a serialized writer, but server-level
read/write sets, semantic conflicts, task replay, and effect handling remain our
responsibility. Another backend can implement the same model.

The transaction unit is one uninterrupted MOO execution segment, from task
start or resumption through return, error, or a semantic suspension such as
`suspend()` or an input wait. At a suspension boundary, validation and commit
occur before the continuation becomes suspended. Resumption begins a new
attempt against current committed state, so reads after resumption may observe
changes committed by other tasks. Locals and VM stack values survive as ordinary
captured Vars; a backend transaction or snapshot does not.

Conflict replay is an implementation detail, not MOO-visible suspension or
preemption. Workers may execute segments concurrently, but each segment runs
uninterrupted until a language-visible boundary or resource-limit abort. On a
validation conflict the server discards attempt-local state and silently
restarts that segment against a newer snapshot.

### Non-suspendable task execution

Native-to-MOO callbacks sometimes require a real result while a foreign C stack
remains active. The VM should have a first-class, nestable non-suspendable task
constraint, provisionally `TASK_NO_SUSPEND`. The prototype now implements a
nestable execution guard and checks it in the central suspension path. Every
path which can semantically
yield—including direct or indirect `suspend()`, input waits, and a native
built-in returning a suspended package—checks it centrally. An attempted yield
raises `E_INVARG` in the prototype; a dedicated error remains a possible ABI
choice. It must never be interpreted as a fabricated
callback result such as integer zero.

This constraint does not prohibit optimistic conflict replay. Resource or tick
exhaustion aborts normally and does not create a suspended continuation. A
foreign invocation remains live only within one attempt; if validation fails,
the containing native operation and its transient foreign state are discarded
and reconstructed while replaying the whole segment. No foreign stack is kept
across replay, suspension, or checkpointing.

## Replay classes

Native and bound operations need explicit semantics:

- Pure: deterministic, read-only, and freely replayable.
- Transactional: reads or mutates attempt-local managed state.
- Recorded: nondeterministic input is journaled and reused on replay.
- Deferred effect: externally visible action occurs only after commit.
- Irrevocable: operation cannot replay; execution must serialize or commit
  before it occurs.

Immutable bytes and struct reads are pure. Builder, stream, lightweight-object,
SQLite-image, and WASM-state mutations are transactional when implemented
through overlays. Time and randomness may be recorded. notify() should be a
deferred effect. External filesystem or network mutation may be irrevocable.

Replaying database code without replaying inputs and suppressing duplicate
effects is incorrect.

## Persistent bound identity

Pointer identity cannot be durable identity in MVCC storage. A persistent bound
value needs:

    stable bound identity
    subtype and owner
    provider schema
    committed revision
    provider blob
    provider references by persistent identity

Wrappers may be interned by stable identity within a process snapshot. Revision
selects transaction-visible state. Immutable payload interning is a separate
storage optimization.

The same identity remains across conflict replay even when its state is rebased
onto a newer committed revision.

## Transactional bound state

The prototype currently resembles:

    Var -> BoundValue wrapper -> mutable provider payload

Unrestricted mutation of shared C payloads is incompatible with isolation and
rollback. The long-term model is:

    stable identity
      committed provider state at revision N
      shared attempt overlay based on revision N

Every alias to one identity within an attempt sees one overlay. Other attempts
continue seeing committed state.

The final API may offer transaction-aware state access or provider begin,
validate, commit, and abort hooks. The exact ABI is open, but the semantic rule
is firm:

> Provider mutation belongs to the current attempt and has explicit commit and
> abort behavior.

Providers may implement overlays with immutable replacement, COW pages,
persistent data structures, patch intervals, or operation journals.

## Effects and nondeterminism

notify() must not send twice after conflict replay. It queues a post-commit
effect. A failed attempt discards the queue.

A future backpressure-aware form of `notify()` may request suspension when the
connection output buffer is full. This could be an option bit or a separate
builtin; it must be a first-class operation rather than each caller manually
chunking large output. Ordinary `notify()` can retain its nonblocking semantics.

A backpressure suspension is a transaction boundary. The attempt first validates
and commits, including an immutable retained output value or server-owned send
continuation. Only post-commit delivery may advance the accepted-byte offset. If
the buffer cannot accept the remainder, the task suspends with that progress and
resumes in a new transaction after capacity becomes available. Conflict replay
therefore occurs before any bytes from that attempt are accepted and cannot
duplicate already accepted output.

The implementation may instead make connection output queues transactional
server state and drain them as deferred effects. In either representation, buffer
reservation, accepted progress, cancellation, disconnect, and partial-write
errors need explicit semantics. A non-suspendable task must receive the defined
would-block error rather than using this operation to yield.

Connection input is harder because data has left the socket buffer. The server
can record consumed input for replay or establish a transaction boundary before
consumption.

Time and randomness can be attempt-journal entries. Replay needs stable operation
ordering and a rule for divergence when the new execution takes another branch.

Irrevocable operations may reduce concurrency. Making that explicit is safer
than treating unrepeatable effects as transactional.

## Provider implications

### Structs

Immutable struct values and layouts are pure and shareable. Builder assignments
and fixed patches form a transaction overlay shared by nested builder aliases.
finish() creates a new immutable value; storing it and sealing the builder
publish atomically.

### Bytes and streams

Bytes are naturally immutable. Streams need versioned buffers or append
journals. Concurrent appends normally conflict; the loser replays after the
winner, producing a deterministic serial ordering unless an explicit merge rule
is defined.

### SQLite

A checkpoint-safe SQLite value uses canonical database-image bytes and a
transient connection cache. Under replay:

    committed image
    -> attempt-local connection
    -> SQL operation
    -> new canonical attempt image
    -> commit or discard

On conflict the handle is discarded and the operation reruns against the newer
image. External files and nondeterministic SQL functions require irrevocable or
recorded semantics. Whole-image serialization is a correct but potentially
expensive starting point; page-level COW can follow.

### WASM

A WASM call operates on attempt-local logical state or a memory overlay. Engine
and JIT objects are caches. Imports are classified as pure, transactional,
recorded, deferred, or irrevocable. Failed attempts discard the runtime state
and replay from a newer committed revision.

### Lightweight objects

Slot mutations use the transaction overlay. Class definitions, permissions, and
method lookup facts belong in the read set. Replay reevaluates them after a
conflict.

## Multithreading and conflicts

Multiple CPU cores are useful only when VM and provider state are isolated.
Independent task attempts can run on workers while a commit coordinator
serializes publication.

A read set includes objects, properties, verbs, property definitions, bound
revisions, permission facts, and dynamic type definitions which influenced the
attempt. A write set contains replacement revisions and deferred effects.

Conflict detection can begin conservatively. False conflicts cost throughput
but preserve correctness. Later implementations can use property-, verb-, or
provider-subrecord granularity.

Authority is transactional input. If ownership or permissions change, replay
must check again rather than reuse stale authorization.

## Garbage collection

MVCC retains old revisions while snapshots or backups can observe them.
Reference counting alone is increasingly insufficient, especially with cyclic
provider graphs.

Persistent roots include committed objects, suspended tasks, active snapshots,
queued effects, dynamic definitions, and retained history. Provider edge
enumeration may support both in-memory cycle collection and backend reachability
GC. Persistence encoding is not a substitute for safe collector traversal.

Old revisions can be reclaimed only after no reader, continuation, backup, or
replication consumer can observe them.

## Bound-core requirements suggested by this experiment

1. Stable bound identity independent of wrapper pointers.
2. Pure canonical provider persistence.
3. A provider snapshot/synchronization contract for unforked checkpoints.
4. Versioned provider state and shared per-attempt overlays.
5. Explicit commit and abort behavior for mutation.
6. Replay/effect classification for native operations.
7. Recorded nondeterminism and post-commit effect delivery.
8. Defined transaction boundaries at suspension and task completion.
9. Provider edge enumeration for tracing and persistent GC.
10. Transactional dynamic subtype registration.
11. Explicit irrevocable-operation behavior.

The current bound implementation remains a useful prototype, but unrestricted
bound_payload() mutation should not be considered the final provider contract.

## Experimental sequence

The checkpoint and backend work can proceed separately:

1. Specify the pure provider snapshot and encoding contract.
2. Exercise UNFORKED_CHECKPOINTS, use the Waterpoint result only as a reference,
   establish a LambdaMOO baseline, and profile the complete pause.
3. Optimize the synchronous full dump and establish a pause budget on
   representative databases and storage.
4. Prototype a full SQLite checkpoint file using the same logical fixture and
   durability requirements; compare write, publication, reload, and file costs.
5. Only if needed, prototype a stable in-memory snapshot for concurrent dump
   encoding.
6. Define task attempt, commit, suspension, and replay independently of the
   checkpoint implementation.
7. Journal reads, writes, nondeterminism, and deferred notifications.
8. Replay a deliberately conflicted pure MOO task.
9. Give stream or struct.builder a shared transactional overlay.
10. Persist stable bound identities and revisions in separate minimal SQLite
    and LMDB live-store experiments.
11. Run read-mostly attempts on worker threads and serialize commits.
12. Exercise bound SQLite and WASM state under forced conflicts.
13. Add reachability accounting for provider edges and old revisions.

Adversarial tests should include notification during replay, consumed input,
permission changes, mutable aliases, suspension after mutation, provider
unavailability, worker cancellation, and dynamic type registration conflicts.

## Open questions

- Is stop-the-world unforked dumping acceptable as an intermediate deployment
  mode, or is a stable concurrent snapshot required immediately?
- What data structures must become immutable/versioned before a checkpoint
  snapshot can coexist with writers?
- What is the transaction boundary for long-running nonsuspending tasks?
- How are replay journal positions matched after branch divergence?
- Do irrevocable operations force commit, acquire a global token, or reject?
- How granular are object and bound conflict keys?
- How are large bytes, streams, SQLite images, and WASM memories chunked?
- How much VM-global state must be split before task attempts can use threads?
- How are old revisions and cyclic provider graphs collected?
- How do replication and migration handle unavailable dynamic providers?

The principal consequence for the current spike is:

> Bound values should be persistent logical identities with replaceable,
> versioned state—not merely wrappers around freely mutable C objects.

That principle improves today's unforked-checkpoint option and preserves a path
toward a transactional, concurrent backend without committing to LMDB now.
