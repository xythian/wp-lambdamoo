//! Experimental Rust replacements for parts of the LambdaMOO server.
//!
//! Each module exports the same C ABI as the C file it replaces, so the
//! server links against this static library in place of that object file.
//! See README.md in this directory for how the build switches work.

#![cfg_attr(not(test), no_std)]

#[cfg(all(feature = "num64", feature = "num32"))]
compile_error!("enable exactly one of the num64 and num32 features");

pub mod ffi;
#[cfg(feature = "utf")]
pub mod utf;

#[cfg(not(test))]
#[panic_handler]
fn on_panic(_: &core::panic::PanicInfo) -> ! {
    ffi::server_panic(c"Rust panic in moo-rs")
}

/// The precompiled core library is built for unwinding and refers to this
/// symbol from code we never reach: we build with panic = "abort", so no
/// unwinding ever starts.
#[cfg(not(test))]
#[no_mangle]
extern "C" fn rust_eh_personality() {}
