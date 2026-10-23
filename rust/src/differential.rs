//! Differential tests: the port against the original utf.c, on every
//! short input and on many random long ones.
//!
//! Run with a configured server tree:
//!     MOO_BUILD_DIR=.. cargo test --release --features differential

use core::ffi::{c_char, c_int, c_void};
use std::ffi::CString;
use std::vec::Vec;

use crate::ffi::Num;
use crate::utf;

extern "C" {
    fn c_get_utf(pp: *mut *const c_char) -> u32;
    fn c_get_utf_call(
        getch: unsafe extern "C" fn(*mut c_void) -> i32,
        data: *mut c_void,
        state: *mut i32,
    ) -> i32;
    fn c_put_utf(pp: *mut *mut c_char, v: u32) -> c_int;
    fn c_utf_byte_index(s: *const c_char, ci: Num) -> Num;
    fn c_utf_byte_range(s: *const c_char, cis: *mut [Num; 2]);
    fn c_utf_char_index(s: *const c_char, bi: Num) -> Num;
    fn c_memo_strlen_utf(s: *const c_char) -> usize;
    fn c_clearance_utf(c: u8) -> usize;
}

#[no_mangle]
extern "C-unwind" fn c_utf_test_panic(_: *const c_char) -> ! {
    panic!("C utf.c panicked")
}

/// Small deterministic generator so failures reproduce.
struct XorShift(u64);

impl XorShift {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }

    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// Bytes near every boundary the decoder cares about.
const EDGE_BYTES: &[u8] = &[
    0x01, 0x41, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf, 0xe0, 0xe1,
    0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf4, 0xf5, 0xf7, 0xf8, 0xfe, 0xff,
];

/// Decode `s` to the end with both versions, comparing every step.
fn compare_decode(s: &[u8]) {
    let cs = CString::new(s).unwrap();
    let (mut pc, mut pr) = (cs.as_ptr(), cs.as_ptr());
    loop {
        let (vc, vr) = unsafe { (c_get_utf(&mut pc), utf::get_utf(&mut pr)) };
        assert_eq!((vc, pc), (vr, pr), "get_utf on {s:02x?}");
        if vc == 0 {
            break;
        }
    }
    unsafe {
        assert_eq!(c_memo_strlen_utf(cs.as_ptr()), utf::memo_strlen_utf(cs.as_ptr()), "{s:02x?}");
    }
}

fn random_string(rng: &mut XorShift, max_len: u64) -> Vec<u8> {
    let len = rng.below(max_len + 1);
    (0..len)
        .map(|_| match rng.below(4) {
            0 => EDGE_BYTES[rng.below(EDGE_BYTES.len() as u64) as usize],
            1 => 0x20 + rng.below(0x5f) as u8,
            _ => 1 + rng.below(255) as u8,
        })
        .collect()
}

/// Character and byte indices worth trying on a string of `len` bytes.
fn interesting_indices(rng: &mut XorShift, len: Num) -> Vec<Num> {
    let mut v = vec![Num::MIN, Num::MIN + 1, -1, 0, 1, 2, len, len + 1, len + 2, Num::MAX - 1, Num::MAX];
    v.extend((0..6).map(|_| rng.below(len as u64 + 4) as Num - 1));
    v
}

#[test]
fn decode_every_string_up_to_three_bytes() {
    let mut s = Vec::with_capacity(3);
    for a in 1..=255u8 {
        compare_decode(&[a]);
        for b in 1..=255u8 {
            compare_decode(&[a, b]);
            for c in 1..=255u8 {
                s.clear();
                s.extend_from_slice(&[a, b, c]);
                compare_decode(&s);
            }
        }
    }
}

#[test]
fn decode_edge_bytes_up_to_five_long() {
    let n = EDGE_BYTES.len();
    let mut s = [0u8; 5];
    for mut i in 0..n.pow(5) {
        for b in s.iter_mut() {
            *b = EDGE_BYTES[i % n];
            i /= n;
        }
        compare_decode(&s);
    }
}

#[test]
fn decode_random_strings() {
    let mut rng = XorShift(0x5eed_0001);
    for _ in 0..200_000 {
        compare_decode(&random_string(&mut rng, 40));
    }
}

#[test]
fn encode_all_code_points_and_beyond() {
    let mut rng = XorShift(0x5eed_0002);
    let values = (0..=0x11_0100u32)
        .chain((0..1_000_000).map(|_| rng.next() as u32))
        .chain([u32::MAX, u32::MAX - 1, 0x7fff_ffff, 0x8000_0000]);
    for v in values {
        let (mut bc, mut br) = ([0x55u8; 8], [0x55u8; 8]);
        let (mut pc, mut pr) = (bc.as_mut_ptr().cast::<c_char>(), br.as_mut_ptr().cast::<c_char>());
        let (rc, rr) = unsafe { (c_put_utf(&mut pc, v), utf::put_utf(&mut pr, v)) };
        let (nc, nr) = unsafe {
            (pc.offset_from(bc.as_mut_ptr().cast()), pr.offset_from(br.as_mut_ptr().cast()))
        };
        assert_eq!((rc, nc, bc), (rr, nr, br), "put_utf({v:#x})");
    }
}

#[test]
fn clearance_every_byte() {
    for c in 0..=255u8 {
        assert_eq!(unsafe { c_clearance_utf(c) }, utf::clearance_utf(c), "{c:#x}");
    }
}

#[test]
fn indices_on_random_strings() {
    let mut rng = XorShift(0x5eed_0003);
    for _ in 0..50_000 {
        let s = random_string(&mut rng, 30);
        let cs = CString::new(s.clone()).unwrap();
        let p = cs.as_ptr();
        let len = s.len() as Num;
        let idx = interesting_indices(&mut rng, len);

        for &ci in &idx {
            let (c, r) = unsafe { (c_utf_byte_index(p, ci), utf::utf_byte_index(p, ci)) };
            assert_eq!(c, r, "utf_byte_index({s:02x?}, {ci})");
        }
        for &a in &idx {
            for &b in &idx {
                let (mut c, mut r) = ([a, b], [a, b]);
                unsafe {
                    c_utf_byte_range(p, &mut c);
                    utf::utf_byte_range(p, &mut r);
                }
                assert_eq!(c, r, "utf_byte_range({s:02x?}, [{a}, {b}])");
            }
        }
        // utf_char_index needs a character start no further than strlen+2.
        let mut starts: Vec<Num> = (1..=len + 2)
            .filter(|&bi| bi > len || !utf::is_cont_byte(s[bi as usize - 1]))
            .collect();
        starts.extend([Num::MIN, -1, 0]);
        if !s.is_empty() && utf::is_cont_byte(s[0]) {
            starts.retain(|&bi| bi <= 1);
        }
        for bi in starts {
            let (c, r) = unsafe { (c_utf_char_index(p, bi), utf::utf_char_index(p, bi)) };
            assert_eq!(c, r, "utf_char_index({s:02x?}, {bi})");
        }
    }
}

struct Feed<'a> {
    bytes: &'a [u8],
    pos: usize,
}

unsafe extern "C" fn feed_getch(data: *mut c_void) -> i32 {
    let feed = &mut *data.cast::<Feed>();
    match feed.bytes.get(feed.pos) {
        Some(&b) => {
            feed.pos += 1;
            b as i32
        }
        None => -1,
    }
}

#[test]
fn callback_decoder_on_random_streams() {
    let mut rng = XorShift(0x5eed_0004);
    for _ in 0..100_000 {
        // The callback decoder sees raw network bytes, NULs included.
        let s: Vec<u8> = (0..rng.below(30))
            .map(|_| match rng.below(3) {
                0 => EDGE_BYTES[rng.below(EDGE_BYTES.len() as u64) as usize],
                _ => rng.below(256) as u8,
            })
            .collect();
        let (mut fc, mut fr) = (Feed { bytes: &s, pos: 0 }, Feed { bytes: &s, pos: 0 });
        let (mut stc, mut str) = (-1, -1);
        for _ in 0..=s.len() + 1 {
            let (vc, vr) = unsafe {
                (
                    c_get_utf_call(feed_getch, (&mut fc as *mut Feed).cast(), &mut stc),
                    utf::get_utf_call(feed_getch, (&mut fr as *mut Feed).cast(), &mut str),
                )
            };
            assert_eq!((vc, stc, fc.pos), (vr, str, fr.pos), "get_utf_call on {s:02x?}");
        }
    }
}

/// Rough timing of the two versions; run with --ignored --nocapture.
#[test]
#[ignore]
fn timing() {
    use std::time::Instant;
    let text: Vec<u8> = "plain ascii text, caf\u{e9}, \u{20ac}20, \u{1f600} "
        .bytes()
        .cycle()
        .take(1 << 20)
        .collect();
    let cs = CString::new(text).unwrap();
    let p = cs.as_ptr();
    let n = unsafe { utf::memo_strlen_utf(p) } as Num;
    let reps = 200;

    let time = |name: &str, f: &dyn Fn() -> u64| {
        let t = Instant::now();
        let mut sink = 0;
        for _ in 0..reps {
            sink ^= f();
        }
        std::println!("{name:28} {:>8.2?} per call  ({sink})", t.elapsed() / reps);
    };
    time("C    memo_strlen_utf", &|| unsafe { c_memo_strlen_utf(p) as u64 });
    time("Rust memo_strlen_utf", &|| unsafe { utf::memo_strlen_utf(p) as u64 });
    time("C    utf_byte_index(mid)", &|| unsafe { c_utf_byte_index(p, n / 2) as u64 });
    time("Rust utf_byte_index(mid)", &|| unsafe { utf::utf_byte_index(p, n / 2) as u64 });
    time("C    utf_byte_range", &|| unsafe {
        let mut r = [n / 3, n / 2];
        c_utf_byte_range(p, &mut r);
        r[1] as u64
    });
    time("Rust utf_byte_range", &|| unsafe {
        let mut r = [n / 3, n / 2];
        utf::utf_byte_range(p, &mut r);
        r[1] as u64
    });
    let bi = unsafe { utf::utf_byte_index(p, n / 2) };
    time("C    utf_char_index(mid)", &|| unsafe { c_utf_char_index(p, bi) as u64 });
    time("Rust utf_char_index(mid)", &|| unsafe { utf::utf_char_index(p, bi) as u64 });
}
