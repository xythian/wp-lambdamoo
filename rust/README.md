# moo-rs: experimental Rust ports of server modules

This directory holds Rust ports of individual C files from the server.
Each port exports the same C ABI as the file it replaces, so the rest of
the server does not change: the Makefile drops the C object and links the
Rust static library instead.

This is an experiment. The default build does not use any of it.

## Ported modules

None yet.

## Building the server with Rust modules

Configure as usual, then name the modules to take from Rust:

```sh
./configure --enable-sz=i64 --enable-unicode ...
make RUST_MODULES=<module> ...
```

This needs a Rust toolchain (`cargo`, 1.77 or later). The Makefile reads
`INT_TYPE_BITSIZE` from the configured headers and builds the library for
the matching `Num` size; `rust_glue.c` makes a mismatch a link error.

`make clean` removes the Rust build directory (`rust-target/`) too.

## Tests

```sh
cd rust && cargo test
```

The server-level check is the lambdamoo-tests suite run against a server
built with `make RUST_MODULES=...`.

## Design notes

- The library is `no_std` and built with `panic = "abort"`, so it adds no
  runtime and no libraries to the link. A Rust panic calls the server's
  own `panic()`, which logs and dumps the database like any other panic.
- The Rust standard library's `compiler_builtins` includes its own
  `sqrt`, `fmod` and other libm functions, which do not set `errno`.
  Linked as a plain archive, they replaced the C library's for the whole
  server, and `sqrt(-1.0)` started raising `E_FLOAT` instead of
  `E_INVARG` (lambdamoo-tests caught it). The Makefile therefore
  partially links the library into `rust_modules.o`, pulling in only what
  the ported modules need, and makes every symbol but their C entry points
  local. Building this way needs GNU `ld` and `objcopy`.
