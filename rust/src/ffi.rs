//! Types and functions shared with the C side of the server.

use core::ffi::CStr;

/// The server's `Num` (see structures.h).
#[cfg(not(feature = "num32"))]
pub type Num = i64;
#[cfg(feature = "num32")]
pub type Num = i32;

pub const NUM_MAX: Num = Num::MAX;

/// Lets rust_glue.c check at link time that `Num` matches the C build.
#[cfg(not(feature = "num32"))]
#[no_mangle]
pub static moo_rs_num_bits_64: u8 = 64;
#[cfg(feature = "num32")]
#[no_mangle]
pub static moo_rs_num_bits_32: u8 = 32;

#[cfg(not(test))]
extern "C" {
    fn panic(message: *const core::ffi::c_char) -> !;
}

/// Shut the server down through its own panic(), which logs, dumps the
/// database and aborts.
#[cfg(not(test))]
pub fn server_panic(message: &CStr) -> ! {
    // SAFETY: panic() takes a NUL-terminated message and never returns.
    unsafe { panic(message.as_ptr()) }
}

/// Unit tests have no server to panic, so unwind and let the test see it.
#[cfg(test)]
pub fn server_panic(message: &CStr) -> ! {
    core::panic!("server panic: {:?}", message)
}
