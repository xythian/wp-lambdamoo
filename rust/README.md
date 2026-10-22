# moo-rs: experimental Rust ports of server modules

This directory holds Rust ports of individual C files from the server.
Each port exports the same C ABI as the file it replaces, so the rest of
the server does not change: the Makefile drops the C object and links the
Rust static library instead.

This is an experiment. The default build does not use any of it.

## Ported modules

| Module | Replaces | Notes |
|--------|----------|-------|
| `utf`  | `utf.c`  | UTF-8 decode/encode and character/byte index translation (Unicode builds only) |
| `net_mplex` | `net_mplex.c` | Waiting for network I/O, with `poll()` instead of `select()` (Linux) |

## Building the server with Rust modules

Configure as usual, then name the modules to take from Rust:

```sh
./configure --enable-sz=i64 --enable-unicode ...
make RUST_MODULES="utf net_mplex"
```

This needs a Rust toolchain (`cargo`, 1.77 or later) and libclang (for
example `libclang-dev`). `build.rs` runs bindgen over the configured
tree's headers (see `wrapper.h`), so `Num`, `Var` and the other C types
the ports use always match the C build: there is no separate setting for
the integer size to get wrong.

`make clean` removes the Rust build directory (`rust-target/`) too.

## Tests

Unit tests (these need a configured server tree too, for the bindings;
`MOO_BUILD_DIR` names it and defaults to the parent directory):

```sh
cd rust && cargo test
```

Differential tests compile the original `utf.c` into the test binary
under `c_`-prefixed names and compare the two implementations on every
string of up to three bytes, on all five-byte strings over a set of
boundary bytes, on random strings, on every code point for encoding, and
on index translation at and around every boundary (including `NUM_MIN`
and `NUM_MAX`). They need a configured server tree for `config.h` and
`options.h`:

```sh
cd rust && MOO_BUILD_DIR=.. cargo test --release --features differential
```

`cargo test --release --features differential timing -- --ignored --nocapture`
prints rough timings of both versions on a 1 MB string.

The server-level check is the lambdamoo-tests suite run against a server
built with `make RUST_MODULES="utf net_mplex"`.

## Design notes

- The library is `no_std` and built with `panic = "abort"`, so it adds no
  runtime and no libraries to the link. A Rust panic calls the server's
  own `panic()`, which logs and dumps the database like any other panic.
- C hands these functions NUL-terminated strings, often in the middle of
  buffers whose length is unknown. Rather than call `strlen()` first,
  which would make index lookups near the start of a long string cost the
  whole string, the port reads through one small `Cursor` type that stops
  at the NUL. That type holds all of the pointer reading; the decoding
  and index logic above it is ordinary safe Rust.
- `utf_char_index()` in C trusts its caller to pass `bi <= strlen + 2`
  and reads backwards from there. The port checks that requirement as it
  reads, so a bad caller panics the server instead of reading past the
  string.
- The Rust standard library's `compiler_builtins` includes its own
  `sqrt`, `fmod` and other libm functions, which do not set `errno`.
  Linked as a plain archive, they replaced the C library's for the whole
  server, and `sqrt(-1.0)` started raising `E_FLOAT` instead of
  `E_INVARG` (lambdamoo-tests caught it). The Makefile therefore
  partially links the library into `rust_modules.o`, pulling in only what
  the ported modules need, and makes every symbol but their C entry points
  local. Building this way needs GNU `ld` and `objcopy`.
- `select()` holds descriptors below `FD_SETSIZE` (1024). Once the server's
  descriptor limit is raised past that, the C server's `FD_SET()` on
  descriptor 1024 trips glibc's fortify check and the server aborts
  without saving the database (lambdamoo-tests NET-100). `net_mplex`
  waits with `poll()`, which has no such limit. `poll()` reports a closed
  descriptor as `POLLNVAL` and returns at once rather than failing with
  `EBADF`; the port turns `POLLNVAL` back into `EBADF`, which is how
  `net_multi.c` knows to find and drop such descriptors. The C
  `MPLEX_STYLE=MP_POLL` code does not, and busy-loops on a closed
  descriptor.
