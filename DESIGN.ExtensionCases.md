# Bound extension contract case studies

This prospective document tests the bound contract against SQLite, WASM, and a
WAIF-like lightweight object. It complements README.Bound, DESIGN.Bound, and
DESIGN.Struct.md.

## Current checkpoint rule

With the current backend, normal checkpoints fork before database save hooks and
provider encoders run in the child. `DESIGN.TransactionalDatabase.md` describes
both unforked checkpoints and a possible MVCC replacement. Today this imposes:
the child while the parent continues. Therefore:

> Encoding must be fork-safe and operate only on canonical value state. It
> must not enter a foreign runtime, acquire its mutexes, contact a worker, or
> advance computation.

Transient handles are caches, never the sole copy of persistent state. A
state-changing operation reaches a safe boundary only after updating canonical
state which the child can encode without calling the foreign library.

A provider operation is either checkpointable, with all continuation state in
ordinary serialized data, or atomic and transient: it cannot suspend and
finishes before control returns to the server loop. MOO-visible values are
presumed checkpointable. Statements, cursors, transactions, execution stacks,
and similar intermediates must not be exposed unless reload semantics exist.

## SQLite

The first provider exposes sqlite.database, a logical database image rather
than a public sqlite3 connection. Persistent state contains canonical serialized
SQLite bytes, options and limits, owner/authority policy, registered-function
metadata, and provider versioning.

An open connection is a transient cache. After each successful mutating atomic
operation, the provider refreshes canonical bytes before returning to MOO.
Encoding reads those bytes and never calls SQLite in the forked child.

A file-backed database is a separate external-resource capability with explicit
path authorization, reopen behavior, replacement detection, and restart
failure semantics. It is not equivalent to the checkpointed in-memory image.

The initial MOO surface uses atomic calls:

    db = bound_type("sqlite.database"):new();
    db:execute(sql, bindings);
    rows = db:query(sql, bindings);
    image = db:bytes();
    results = db:batch({{sql1, args1}, {sql2, args2}});

Queries return bounded materialized results. The first version does not expose
connections, prepared statements, row cursors, incremental blobs, or open
transactions. A task cannot hold a SQLite transaction across suspension.

SQL-function registration metadata may persist, but executing a MOO UDF is a
foreign-stack problem: SQLite calls it inside sqlite3_step(). The generic
resumable bound call cannot unwind and restore that stack. The safe first
version omits MOO UDF execution.

A later worker/broker prototype may block a worker in SQLite while the main
thread runs a nonsuspending callback. It must reject recursive database use and
suspension and prevent checkpoint fork until the operation ends. That prototype
may motivate a bounded transient no-checkpoint region with cancellation,
duration, and checkpoint-starvation rules; the generic core should not promise
one before the worker design validates it.

Nonsuspending callback execution should be a first-class, nestable VM task
constraint. Every direct or indirect semantic yield raises instead of being
converted into a default callback result. Optimistic conflict replay remains
permitted and invisible to MOO: the server discards the attempt-local SQLite
connection and restarts the entire uninterrupted MOO segment. It never retains
the blocked worker or SQLite C stack across replay or a suspension boundary.

SQLite therefore requires canonical fork-safe images, rebuildable handle
caches, atomic mutation/canonicalization, and no surfaced intermediate native
objects initially. It does not justify nonpersistent bound values.

## WASM

Likely types are wasm.module, wasm.instance, and wasm.bound_type.

A module persists canonical WASM bytes, validation/import policy, ABI metadata,
and a content hash. JIT code and compiled engine artifacts are transient caches.

An instance persists only portable logical state: module reference, linear
memory, mutable globals, portable table entries, provider host state, fuel, and
authority policy. Engine stores, native stacks, JIT pointers, and borrowed
memory views never persist.

Initial calls are synchronous, fuel-limited, and atomic:

    result = instance:call("parse", args);

They cannot suspend or call arbitrary MOO verbs. Successful mutation updates
canonical state before return, and encoding never enters the WASM engine.
Traps, fuel exhaustion, and forbidden imports become structured MOO errors.

Cooperative suspension requires a runtime capable of exporting a portable
continuation containing its program counter, value/call stacks, locals, and
host-call state. A raw engine stack is not such a continuation. The generic
bound continuation can carry portable state only after a runtime supplies it.

A WASM-defined bound subtype is a persistent definition containing a stable
name, module and ABI version, export names, payload schema, authority, and
limits. One compiled adapter provider dispatches these definitions; WASM does
not install C callbacks.

Values may load opaque before their definitions are available. A post-load
phase validates definitions, registers stable names, and lets normal late
binding decode retained values. Conflicts, missing modules, ABI mismatch, or
revoked authority leave values unavailable.

This requires post-load dynamic registration, namespace ownership, conflict and
revocation rules, deterministic unavailable behavior, bounded blob/reference
host APIs, and canonical state which can dump without executing WASM.

## WAIF-like lightweight objects

This is not a proposal to replace or ship an alternative to WAIFs. It asks
whether bound types existing first could have supported equivalent lightweight
objects.

Provisional types are lightweight.type and lightweight.object. A type refers to
a MOO class, property definitions/defaults, inheritance/version metadata, and
method lookup policy. An object stores owner, class/type, slot values,
invalidation state, and a property-definition generation.

Required behavior includes dynamic properties and enumeration, class evolution
with slot migration, MOO verb lookup beginning at a class object, secure method
activation with the bound receiver as this, permissions, invalidation,
identity/graph persistence, and use in suspended tasks.

Dynamic property hooks cover slots. Generation-stamped slot maps can update
lazily when class definitions change.

Method dispatch is the larger core gap. The VM needs a provider hook separating:

    lookup object     inheritance search origin
    secure this       lightweight bound receiver
    programmer        normal authority
    verb and args     ordinary activation inputs

For ordinary invocation this creates a normal serializable MOO activation and
need not resume native code afterward. Native callbacks use the more general
bound_call_moo_verb continuation.

The core currently fixes class reflection to the compiled subtype handle.
Lightweight objects and structured values both need semantic class/type
reflection. The generic answer is separate bound-class reflection plus a
provider semantic-type/class hook.

Persistence can reconstruct self and mutual references because wrappers are
registered before sidecars. That proves identity, not reclamation. Strong
provider-held Var edges form ordinary cycles in lightweight object graphs.
Explicit cycle breaking is not WAIF-equivalent.

Full equivalence therefore requires cycle-aware reclamation: tracing over
provider-enumerated Var edges, trial deletion, or a sufficient weak-edge model.
An optional edge-enumeration hook is the most general future contract;
persistence encoding cannot substitute for collector traversal.

The thought experiment concludes “almost, but not yet.” Full WAIF-like support
needs dynamic properties, secure MOO method dispatch, semantic class reflection,
class evolution, invalidation, and cycle-aware provider edge traversal. Those
hooks also benefit structs, WASM types, JSON callbacks, and other providers.
