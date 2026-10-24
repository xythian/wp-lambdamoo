# Structured bytes and in-database Kaitai design

This document proposes a structured-byte provider built on bound values and
the bytes/stream provider, and an in-database Kaitai compiler layered on it. It
is prospective design, not an implemented compatibility promise.

The central rule is:

> Native code records and enforces a resolved byte layout. Generated MOO code
> decides which conditional layout to resolve.

This avoids implementing KSY parsing or a second general-purpose expression
language in C while retaining lazy access, safe persistence, and efficient
fixed-layout editing.

## Foundations

The bound core supplies registration, dispatch, ownership, lifetime,
reflection, and persistence for named subtypes of `TYPE_BOUND`. Its envelope
combines provider bytes with core-serialized `Var` references, preserves
wrapper aliases and provider graphs, and retains unavailable values opaquely.

The separate bytes/stream provider supplies immutable `bytes`, mutable
`stream` values, zero-copy sealing into immutable storage, retained output
views, and a connection input sink.

The struct provider is another bound extension. It depends on bytes/stream for
construction, but consumes input through a generic retained, seekable byte
source from its first version. Contiguous bytes are the first implementation;
the same capability admits segmented COW buffers, mapped files, SQLite blobs,
and immutable WASM snapshots without revising the struct contract.

## Goals and layering

The provider should define structures using serializable MOO descriptors,
expose runtime fields through properties and indexing, decode retained source
bytes lazily, support nested values and sequences, construct through builders,
patch fixed layouts with COW overlays, and persist without running MOO during
database load.

It leaves conditional and variable-layout reconstruction to
MOO code. It should not expose raw C structs or pointers, call MOO
from lazy property lookup, or implement every Kaitai processor.

```text
bound core
├── bytes/stream provider
└── structured-bytes provider
    ├── descriptors and primitive codecs
    ├── bounded cursor and resolved-layout recorder
    ├── immutable structured views
    └── builders and fixed-layout overlays

in-database Kaitai compiler
├── parses KSY or another schema representation
├── emits MOO parsing and building verbs
└── uses the structured-bytes provider as its runtime
```

Kaitai is one consumer. Packet formats, file headers, SQLite projections,
application protocols, and WASM layouts can use the substrate directly.

## Native concepts

Provisional bound subtypes are:

```text
struct.type       immutable validated descriptor or codec vocabulary
struct.value      immutable source plus a resolved field layout
struct.builder    mutable construction or edit session
struct.sequence   optional lazy indexed repeated-field view
```

Dynamic parsing also needs a bounded cursor and mutable layout recorder. These
may be separate `struct.cursor` and `struct.layout` values or two views of one
session. The session is explicit data, not retained native control flow, and
must survive suspension, checkpoint, and reload like other MOO-visible values.

### Types and codecs

A minimal native vocabulary includes integers with width and byte/bit order,
bit fields, floats, booleans, enums, bounded bytes, encoded/padded/terminated
strings, nested type references, fixed arrays, validations, and write policy.

Conditions, switches, and repeats are not native expressions.
MOO code will resolve them into concrete layouts during parsing.

An entirely static descriptor is a distinct fast path. `struct.type` precomputes
and owns its immutable field table, shared by every value of that type. Parsing
constructs a view without allocating a per-value layout or running MOO.

A dynamic schema shares static descriptor fragments but records its selected
fields, offsets, lengths, and nested choices per value. Equal resolved layouts
may later be interned, but sharing is an optimization, not an identity promise.

A semantic struct type differs from the compiled bound subtype. Every value has
bound class `struct.value`, but MOO also needs:

```moo
value.type
value.type.name
value.type.schema_hash
```

The core should not overload its existing `.class` reflection for this.

### Cursor and resolved layout

Generated parsing starts a bounded session over immutable bytes:

```moo
session = bound_type("struct.layout"):new(source, packet_type);
cursor = session.cursor;

version = cursor:field("version", "u1");
length = cursor:field("length", "u2be");
cursor:field("payload", {"bytes", length});

packet = session:finish();
```

`cursor:field()` both decodes what the parser needs and records:

```text
field name
codec or nested type
resolved byte and bit offset
resolved byte and bit length
nested layout or sequence metadata
read/write policy
structural dependency flags
```

Cursor operations include bounded seek, alignment, remaining length,
substreams, and explicit position. Arithmetic is overflow-checked and confined
to the session's source range.

### Immutable values and dynamic access

A `struct.value` retains semantic type, immutable source, bounded root range,
resolved field tree, and an optional decoded cache. Caches are derived state
and do not affect equality or serialized meaning.

Nested values retain source and layout directly rather than retaining parent
wrappers. This adapts the useful MSB parent-view idea to immutable storage
without wrapper ownership chains.

Runtime fields require dynamic bound hooks:

```c
enum error (*get_property)(void *, const char *, Var *, Objid);
enum error (*put_property)(void *, const char *, Var, Objid);
enum error (*property_names)(void *, Var *, Objid);
```

Indexing hooks are needed for sequences and builder arrays. A path API can ship
first:

```moo
packet:get({"header", "message_type"});
packet:get({"entries", 2, "flags"});
```

Later this becomes `packet.header.message_type` and
`packet.entries[2].flags`. Absent conditional fields return `E_PROPNF` and
are absent from property enumeration.

## Conditions in generated MOO

An in-database compiler can generate a normal parsing verb:

```moo
session = bound_type("struct.layout"):new(source, packet_type);
cursor = session.cursor;

version = cursor:field("version", "u1");
if (version >= 2)
  flags = cursor:field("flags", "u2be");
endif

kind = cursor:field("kind", "u1");
if (kind == 1)
  cursor:field("payload", payload_type_a);
elseif (kind == 2)
  cursor:field("payload", payload_type_b);
else
  cursor:field("payload", {"bytes", cursor:remaining()});
endif

return session:finish();
```

The value records only the selected layout and does not reevaluate conditions
during access. MOO orchestrates synchronous native primitives, so conditions
use normal ticks, permissions, exceptions, and suspension. The provider does
not initially require `bound_call_moo_verb`.

Generated verbs can implement conditions, switches, repeats, calculated sizes
and positions, validation, eager computed instances, and MOO byte processors.
The compiler must generate code from a validated AST rather than interpolate
untrusted schema text. Compiler authority and generated-verb ownership must be
explicit.

### Structural dependencies

Native code does not need a condition expression to read a resolved value, but
it must know which fields control layout. Generated code records that:

```moo
has_options = cursor:field(
    "has_options",
    "u1",
    {"layout_affects", {"options"}}
);

session:condition({
    "inputs", {"kind"},
    "affects", {"payload"}
});
```

Initially every field used by a condition, switch, repeat, size, or position
expression can be marked structural. This may reject safe patches but cannot
silently corrupt layout.

## Builders

One API covers two strategies:

```moo
builder = type:builder();            /* logical construction */
builder = value:builder();           /* auto */
builder = value:builder("fixed");    /* require overlay editing */
builder = value:builder("rebuild");  /* force reconstruction */

builder.field = new_value;
builder:set({"header", "flags"}, new_value);
result = builder:finish();
```

A `struct.value` is always immutable. Builders never modify their source and
failed edits are atomic.

### Logical construction

A logical builder stores assignments. Generated code decides conditions, writes
discriminators and lengths, chooses nested layouts, and serializes the result.
This handles variable fields, changed array counts, conditional insertion,
layout-changing switches, and dependent offsets, lengths, or checksums.

The provider supplies checked primitive encoders and a result buffer; generated
MOO supplies schema control flow.

### Fixed-layout COW mode

A fixed editor contains:

```text
original immutable bytes
resolved layout
ordered non-overlapping patches: offset, width, replacement bytes
cache invalidation state
```

Reads consult patches before source. A four-byte edit records four replacement
bytes without copying or mutating the source.

A field is patchable when its resolved offset and width remain stable, its
encoding is local, it cannot move other fields, it is not layout-controlling,
and affected derived fields are understood or excluded.

```text
fixed u16 or padded string              patchable
bit field in a fixed integer            patchable by read/modify/write
variable-length string                  rebuild required
array count                             rebuild required
switch discriminator                    rebuild required
conditional-presence controller         rebuild required
```

With contiguous bytes, `finish()` performs one materialization copy. Future
segmented COW bytes can share untouched blocks without changing the API.

Native auto mode must not invoke schema code for a structural edit. It reports
that rebuilding is required; an in-db wrapper chooses reconstruction:

```moo
if (builder:patchable(path))
  builder:set(path, value);
  return builder:finish();
else
  return this:rebuild_with(original, path, value);
endif
```

### Nested builder views

Nested wrappers retain one non-MOO edit session plus a path:

```moo
header = builder.header;
header.message_type = 3;
```

They do not retain parent/child wrappers. Aliases observe the same edits without
creating wrapper cycles. Builder views are live; immutable value views are
snapshots; `finish()` validates, returns a new value, and seals the builder.

## Bytes and byte sources

A retained seekable byte source is a first-version requirement, not a future
generalization. The struct provider must not access the private `ByteStore` in
`ext-bound-stream.c`; it requests this capability through the bound interface.

The bytes provider now exposes extension-facing constructors with precise
ownership in `ext-bound-stream.h`:

```c
Var make_bound_bytes_copy(Objid, const void *, size_t);
Var make_bound_bytes_take(Objid, char *, size_t,
                          void (*release)(void *), void *token);
```

The bound core exposes a retained seekable source:

```c
typedef struct {
    size_t length;
    enum error (*read_at)(void *token, size_t offset,
                          void *destination, size_t length);
    void (*release)(void *token);
    void *token;
} BoundByteSource;
```

`make_bound_bytes_copy()` copies borrowed input. `make_bound_bytes_take()`
transfers lifetime to the store and invokes an optional token release callback
after its last retained user. Without a callback, the data must use the stream
allocator. Optional slices remain prospective.

`BoundTypeDef` now has the optional `byte_source` hook and the core provides the
checked `bound_byte_source()` accessor. Bytes implements bounds-checked
`read_at` and retention; sealed streams expose the same immutable store, while
open streams reject the capability. Struct values will retain the source token
while any lazy child can read it.

## Persistence

Database loading must not execute schema parsers or generated verbs. An
immutable value persists its resolved layout:

```text
blob:
  provider format version
  root offset and length
  resolved field tree and codec IDs
  patchability and dependency flags

references:
  semantic struct.type
  immutable source bytes
  retained cached or synthetic MOO values
```

Bound loading preserves aliases and graphs. An unavailable struct provider
leaves the representation opaque.

Builders are persistable from the first version. Their envelope stores type,
source, logical assignments, patches, mode, sealed state, and view path.
Multiple nested builder wrappers reload sharing the same edit session, and
ordinary wrapper aliases retain identity.

Cursor and layout sessions are also persistable. Their state is data: source,
semantic type, current offset and bounds, partial resolved layout, dependency
metadata, and referenced decoded values. A suspended generated MOO parser
therefore resumes after reload with the same session. No C stack address,
borrowed buffer pointer, or process-local token may be part of that state;
retained byte sources and graph edges use bound persistence references.

This follows the ordinary MOO checkpoint contract. Only intrinsically external
resources such as live network connections may fail across restart. A builder
or parser session backed by persistent values is not such a resource and must
not block a checkpoint or be invalidated merely because it is unfinished.

## Network framing and incremental use

Incremental message handling belongs in MOO. The native provider parses a
bounded snapshot or view; it does not own connection-reading state. A framing
verb accumulates bytes and invokes parsing once a message may be complete.

The initial flow is complete-buffer parsing:

```moo
stream = bound_type("stream"):new();
stream:read(connection);
packet = schema:parse(stream:seal());
```

For a static type, the shared descriptor can report its exact required extent.
For a dynamic type, generated MOO inspects prefix fields and decides whether
enough input exists. Reading beyond the source produces a distinguishable
incomplete-input error containing the requested extent, so MOO can retain the
stream and retry. Small pure parsers may restart; a compiler avoiding repeated
work can generate an explicit MOO state machine retaining scalar state, an
offset, and its persistent cursor/layout session. This requires checkpointable
data but no checkpointable native parser stack or continuation.

## Permissions, quotas, and errors

Schemas are executable resource descriptions. Limits are required for source
length, reads, seeks, nesting, fields, sequences, allocation, cache and layout
size, patches, generated ticks, imports, and processors.

All offset arithmetic is checked before addition or multiplication. Errors
should include field path and source offset. Mutation follows bound owner/wizard
control. Public reads must not leak referenced values with stricter permissions.

## Required extension surfaces

1. Dynamic property get, put, and enumeration hooks.
2. Dynamic indexing, indexed assignment, and length hooks.
3. A required generic retained byte-source capability with seekable reads,
   optional contiguous views, and slices.
4. A public immutable-bytes constructor owned by the bytes provider.
5. Semantic-type reflection separate from bound subtype class.

Native-to-MOO continuations are not initially required because generated MOO
orchestrates synchronous native operations.

## Implementation sequence

1. Add dynamic property hooks. *(Implemented: get, put, and enumeration.)*
2. Add the generic byte-source hook and implement it for bytes and sealed
   streams. *(Implemented, including extension-facing bytes constructors.)*
3. Implement primitive codecs and bounded cursor operations. *(Implemented as
   the persistent `struct.cursor` provider, including bounded child cursors and
   byte extraction.)*
4. Implement resolved layouts and lazy immutable values.
5. Ship path access before VM indexing hooks.
6. Implement persistence for layout sessions and shared builder edit sessions,
   including reload of nested views and suspended parser tasks. *(Cursor
   persistence and source alias preservation are implemented.)*
7. Implement fixed-layout overlays and immutable `finish()`.
8. Add logical construction primitives for generated builders.
9. Prototype an in-db compiler for integers, fixed bytes, nested types, `if`,
   and `switch-on`.
10. Add arrays, repeats, strings, validation, and calculated offsets.
11. Consider processors, lazy MOO-computed instances, and optional native
    incremental acceleration.

The first vertical test parses a conditional packet, exposes selected fields,
patches a nonstructural fixed field, rejects a discriminator edit in fixed mode,
rebuilds it through generated MOO, and survives dump/reload with aliases intact.

## Open decisions

- Whether cursor and layout are one subtype or two.
- Whether `struct.type` contains only codecs or reusable static fragments.
- Descriptor representation before a general map type exists.
- Compact provider layout bytes versus referenced MOO layout data.
- Enum and validation error representation.
- Whether `finish()` seals builders or permits repeated snapshots.
- Equality by type identity, schema hash, or structural equivalence.
- Conservatism of structural-dependency marking.
- When segmented byte sources justify replacing contiguous bytes.

None requires KSY parsing in C. Native code remains responsible for safe codecs,
concrete layouts, immutable views, and controlled construction; in-database
compiler verbs own schema-level control flow.
