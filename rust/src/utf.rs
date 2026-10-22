//! UTF-8 handling for Unicode builds: a port of utf.c.
//!
//! The decoding rules are those of utf.c.  A malformed sequence decodes
//! to U+FFFD, and the byte that broke it is left to start the next
//! character.  Overlong forms, surrogates and code points past U+10FFFF
//! are malformed.
//!
//! The C callers hand us NUL-terminated strings, often in the middle of
//! long buffers whose length we do not know, so most of the code works
//! on a [`Cursor`] that reads up to the NUL rather than on a slice.  All
//! of the pointer reading is in that one type.

use core::ffi::{c_char, c_int, c_void};
#[cfg(test)]
use core::ffi::CStr;

use crate::ffi::{server_panic, Num, NUM_MAX};

pub const INVALID_RUNE: u32 = 0xfffd;
const EOF: i32 = -1;

pub fn is_cont_byte(c: u8) -> bool {
    c & 0xc0 == 0x80
}

/// A source of bytes that can be looked at before being taken, so that
/// a byte which ends a malformed sequence early is not lost.
trait Bytes {
    /// The next byte, or a negative value at the end of input.
    fn peek(&mut self) -> i32;
    fn bump(&mut self);
}

/// Decode the rest of a character whose first byte, `lead`, has already
/// been taken from `src`.
fn decode_after_lead<B: Bytes>(lead: u32, src: &mut B) -> u32 {
    let (mut v, conts, min) = match lead {
        0x00..=0x7f => return lead,
        0x80..=0xbf => return INVALID_RUNE,
        0xc0..=0xdf => (lead & 0x1f, 1, 0x80),
        0xe0..=0xef => (lead & 0x0f, 2, 0x800),
        0xf0..=0xf7 => (lead & 0x07, 3, 0x10000),
        _ => return INVALID_RUNE,
    };
    for _ in 0..conts {
        let c = src.peek();
        if !(0x80..=0xbf).contains(&c) {
            return INVALID_RUNE;
        }
        src.bump();
        v = (v << 6) | (c as u32 - 0x80);
    }
    if v < min || (0xd800..=0xdfff).contains(&v) || v > 0x10ffff {
        INVALID_RUNE
    } else {
        v
    }
}

/// A read position in a NUL-terminated C string.
///
/// The cursor never moves past the terminating NUL except by taking it
/// as a character, which is what get_utf() has always done and what the
/// index arithmetic below depends on.
#[derive(Clone, Copy)]
pub struct Cursor {
    start: *const u8,
    p: *const u8,
}

impl Cursor {
    /// # Safety
    /// `s` must point into a NUL-terminated string that stays alive and
    /// unchanged while the cursor is used.
    pub unsafe fn new(s: *const c_char) -> Self {
        Cursor { start: s.cast(), p: s.cast() }
    }

    /// Bytes taken since the cursor was made.
    pub fn offset(&self) -> usize {
        // SAFETY: p only moves forward from start within one string.
        unsafe { self.p.offset_from(self.start) as usize }
    }

    pub fn as_ptr(&self) -> *const c_char {
        self.p.cast()
    }

    /// Take one character.  Returns 0, having taken the NUL, at the end.
    pub fn next_char(&mut self) -> u32 {
        let lead = self.peek() as u32;
        self.bump();
        decode_after_lead(lead, self)
    }
}

impl Bytes for Cursor {
    fn peek(&mut self) -> i32 {
        // SAFETY: by the contract of new(), every byte up to and including
        // the NUL is readable, and decoding never bumps past a NUL because
        // NUL is not a continuation byte.
        unsafe { *self.p as i32 }
    }

    fn bump(&mut self) {
        // SAFETY: as for peek(); we only bump past a byte we have read.
        self.p = unsafe { self.p.add(1) };
    }
}

/// Bytes pulled from a caller-supplied callback, with the one byte of
/// push-back kept in the caller's `state` (-1 when empty).
struct Callback<'a> {
    getch: unsafe extern "C" fn(*mut c_void) -> i32,
    data: *mut c_void,
    pending: &'a mut i32,
}

impl Bytes for Callback<'_> {
    fn peek(&mut self) -> i32 {
        if *self.pending == -1 {
            // SAFETY: the callback and its data come from the C caller.
            *self.pending = unsafe { (self.getch)(self.data) };
        }
        *self.pending
    }

    fn bump(&mut self) {
        *self.pending = -1;
    }
}

/// Number of bytes in the character that starts with byte `c`, for a
/// well-formed character (1 for a byte that cannot start one).
pub fn clearance(c: u8) -> usize {
    match c {
        0xc0..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf7 => 4,
        _ => 1,
    }
}

/// Encode `v` into `out`, returning the number of bytes written, or None
/// for a surrogate or a value past U+10FFFF.
pub fn encode(v: u32, out: &mut [u8; 4]) -> Option<usize> {
    match v {
        0..=0x7f => {
            out[0] = v as u8;
            Some(1)
        }
        0x80..=0x7ff => {
            out[0] = 0xc0 | (v >> 6) as u8;
            out[1] = 0x80 | (v & 0x3f) as u8;
            Some(2)
        }
        0xd800..=0xdfff => None,
        0x800..=0xffff => {
            out[0] = 0xe0 | (v >> 12) as u8;
            out[1] = 0x80 | ((v >> 6) & 0x3f) as u8;
            out[2] = 0x80 | (v & 0x3f) as u8;
            Some(3)
        }
        0x10000..=0x10ffff => {
            out[0] = 0xf0 | (v >> 18) as u8;
            out[1] = 0x80 | ((v >> 12) & 0x3f) as u8;
            out[2] = 0x80 | ((v >> 6) & 0x3f) as u8;
            out[3] = 0x80 | (v & 0x3f) as u8;
            Some(4)
        }
        _ => None,
    }
}

/// Number of bytes before the character `ci` (1-based) starts, plus one;
/// see utf_byte_index() in utf.h for how indices outside the string map.
pub fn byte_index(mut s: Cursor, mut ci: Num) -> Num {
    if ci <= 1 {
        return ci;
    }
    loop {
        ci -= 1;
        if s.next_char() == 0 || ci <= 1 {
            break;
        }
    }
    let taken = s.offset() as Num;
    if ci > NUM_MAX - taken {
        NUM_MAX
    } else {
        taken + ci
    }
}

/// Translate both character indices in `cis` to byte indices in one pass.
pub fn byte_range(mut s: Cursor, cis: &mut [Num; 2]) {
    // Visit the indices in non-decreasing order; those <= 0 stay as they are.
    let order = if cis[0] > cis[1] { [1, 0] } else { [0, 1] };
    let mut next = order.iter().position(|&i| cis[i] > 0).unwrap_or(2);

    let mut cn: Num = 1;
    loop {
        while next < 2 && cis[order[next]] == cn {
            cis[order[next]] = s.offset() as Num + 1;
            next += 1;
        }
        if next == 2 {
            return;
        }
        cn += 1;
        if s.next_char() == 0 {
            break;
        }
    }

    // The rest are past the end; NUM_MAX is past the end of any string.
    let extra = s.offset() as Num - cn + 1;
    for &i in &order[next..] {
        cis[i] = if cis[i] > NUM_MAX - extra { NUM_MAX } else { cis[i] + extra };
    }
}

/// Character index of the character starting at byte `bi` (1-based).
///
/// utf.c requires `bi <= strlen(s) + 2` and trusts the caller.  Here the
/// requirement is checked while reading, so a bad caller panics the server
/// instead of reading past the string.
pub fn char_index(s: *const c_char, bi: Num) -> Num {
    if bi <= 1 {
        return bi;
    }
    // Count the character starts among the bytes before bi, checking
    // for the NUL as we go; only the last of them may be the NUL itself.
    let len = (bi - 1) as usize;
    let mut bytes = Cursor { start: s.cast(), p: s.cast() };
    if is_cont_byte(bytes.peek() as u8) {
        server_panic(c"UTF_CHAR_INDEX given malformed utf8");
    }
    let mut starts: Num = 0;
    for i in 0..len {
        let b = bytes.peek() as u8;
        if b == 0 && i + 1 < len {
            server_panic(c"UTF_CHAR_INDEX given index past end of string");
        }
        starts += !is_cont_byte(b) as Num;
        bytes.bump();
    }
    1 + starts
}

/* ---------------------------------------------------------------------
 * C ABI, matching the declarations in utf.h
 */

/// # Safety
/// `*pp` must point into a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn get_utf(pp: *mut *const c_char) -> u32 {
    let mut s = Cursor::new(*pp);
    let v = s.next_char();
    *pp = s.as_ptr();
    v
}

/// # Safety
/// `state` must be valid and `getch(data)` must be safe to call.
#[no_mangle]
pub unsafe extern "C" fn get_utf_call(
    getch: unsafe extern "C" fn(*mut c_void) -> i32,
    data: *mut c_void,
    state: *mut i32,
) -> i32 {
    let mut src = Callback { getch, data, pending: &mut *state };
    let lead = src.peek();
    if lead == EOF {
        return EOF;
    }
    src.bump();
    decode_after_lead(lead as u32, &mut src) as i32
}

/// # Safety
/// `*pp` must have room for the encoded character (up to 4 bytes).
#[no_mangle]
pub unsafe extern "C" fn put_utf(pp: *mut *mut c_char, v: u32) -> c_int {
    let mut buf = [0u8; 4];
    match encode(v, &mut buf) {
        Some(n) => {
            core::ptr::copy_nonoverlapping(buf.as_ptr(), (*pp).cast::<u8>(), n);
            *pp = (*pp).add(n);
            0
        }
        None => -1,
    }
}

/// # Safety
/// `s` must point to a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn utf_byte_index(s: *const c_char, ci: Num) -> Num {
    byte_index(Cursor::new(s), ci)
}

/// # Safety
/// `s` must point to a NUL-terminated string and `cis` to two Nums.
#[no_mangle]
pub unsafe extern "C" fn utf_byte_range(s: *const c_char, cis: *mut [Num; 2]) {
    byte_range(Cursor::new(s), &mut *cis)
}

/// # Safety
/// `s` must point to a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn utf_char_index(s: *const c_char, bi: Num) -> Num {
    char_index(s, bi)
}

/// # Safety
/// `s` must point to a NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn memo_strlen_utf(s: *const c_char) -> usize {
    let mut cur = Cursor::new(s);
    let mut n = 0;
    while cur.next_char() != 0 {
        n += 1;
    }
    n
}

#[no_mangle]
pub extern "C" fn clearance_utf(c: u8) -> usize {
    clearance(c)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decode_all(s: &CStr) -> std::vec::Vec<u32> {
        let mut cur = unsafe { Cursor::new(s.as_ptr()) };
        let mut out = std::vec::Vec::new();
        loop {
            match cur.next_char() {
                0 => return out,
                v => out.push(v),
            }
        }
    }

    #[test]
    fn decodes_well_formed() {
        assert_eq!(decode_all(c"a\u{e9}\u{20ac}\u{1f600}"), [0x61, 0xe9, 0x20ac, 0x1f600]);
    }

    #[test]
    fn short_sequence_keeps_the_byte_that_broke_it() {
        assert_eq!(decode_all(c"\xe3\x80A"), [INVALID_RUNE, 0x41]);
    }

    #[test]
    fn rejects_overlong_surrogate_and_out_of_range() {
        assert_eq!(decode_all(c"\xc0\xaf"), [INVALID_RUNE]);
        assert_eq!(decode_all(c"\xed\xa0\x80"), [INVALID_RUNE]);
        assert_eq!(decode_all(c"\xf4\x90\x80\x80"), [INVALID_RUNE]);
    }

    #[test]
    fn char_index_round_trips_byte_index() {
        let s = c"a\u{e9}\u{20ac}\u{1f600}z";
        for ci in 1..=6 {
            let bi = unsafe { utf_byte_index(s.as_ptr(), ci) };
            assert_eq!(char_index(s.as_ptr(), bi), ci);
        }
    }

    #[test]
    #[should_panic(expected = "past end")]
    fn char_index_past_end_panics() {
        char_index(c"ab".as_ptr(), 10);
    }
}
