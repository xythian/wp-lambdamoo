//! Types and functions shared with the C side of the server.
//!
//! The declarations come from the server's own headers (see build.rs), so
//! `Num`, `Var` and the rest always match the configured C build.

use core::ffi::CStr;

#[allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

pub use bindings::*;

pub const NUM_MAX: Num = Num::MAX;

const _: () = assert!(core::mem::size_of::<Num>() * 8 == INT_TYPE_BITSIZE as usize);

/// Shut the server down through its own panic(), which logs, dumps the
/// database and aborts.
#[cfg(not(test))]
pub fn server_panic(message: &CStr) -> ! {
    // SAFETY: panic() takes a NUL-terminated message and never returns.
    unsafe { bindings::panic(message.as_ptr()) }
}

/// Unit tests have no server to panic, so unwind and let the test see it.
#[cfg(test)]
pub fn server_panic(message: &CStr) -> ! {
    core::panic!("server panic: {:?}", message)
}
