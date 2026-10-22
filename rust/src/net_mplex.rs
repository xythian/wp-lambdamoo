//! Waiting for network I/O: a port of the multiplexer in net_mplex.c.
//!
//! The C server waits with select(), whose fd_set holds descriptors
//! below FD_SETSIZE (1024 with glibc).  FD_SET() on a larger descriptor
//! writes past the end of the set, so a server with more than about a
//! thousand connections, listeners and registered descriptors corrupts
//! its own memory.  This port waits with poll(), which takes any number
//! of descriptors.
//!
//! It keeps select()'s contract with net_multi.c.  A descriptor closed
//! out from under the server makes the wait fail with EBADF, which is how
//! net_multi.c knows to look for and drop such descriptors; poll() would
//! report it as POLLNVAL and succeed, so the port turns that back into
//! EBADF.  Readiness is what select() reports for the same poll() events.

use core::ffi::{c_int, c_uint};
use core::ptr;

use libc::{nfds_t, pollfd, POLLERR, POLLHUP, POLLIN, POLLNVAL, POLLOUT};

use crate::ffi::server_panic;
use crate::global::Global;

/// What select() counts as readable and writable (Linux fs/select.c).
const READABLE: i16 = POLLIN | libc::POLLRDNORM | libc::POLLRDBAND | POLLHUP | POLLERR;
const WRITABLE: i16 = POLLOUT | libc::POLLWRNORM | libc::POLLWRBAND | POLLERR;

/// A growable array in C heap memory.  The library is no_std without an
/// allocator, and this is the only growable storage it needs.
struct Buf<T: Copy> {
    ptr: *mut T,
    cap: usize,
}

impl<T: Copy> Buf<T> {
    const fn new() -> Self {
        Buf { ptr: ptr::null_mut(), cap: 0 }
    }

    /// Make room for at least `want` elements; new elements are `fill`.
    fn reserve(&mut self, want: usize, fill: T) {
        if want <= self.cap {
            return;
        }
        let cap = want.max(16).next_power_of_two();
        let bytes = cap
            .checked_mul(core::mem::size_of::<T>())
            .unwrap_or_else(|| server_panic(c"net_mplex: too many descriptors"));
        // SAFETY: ptr is null or came from realloc with the old capacity.
        let new = unsafe { libc::realloc(self.ptr.cast(), bytes) }.cast::<T>();
        if new.is_null() {
            server_panic(c"net_mplex: out of memory");
        }
        for i in self.cap..cap {
            // SAFETY: i < cap, inside the new allocation.
            unsafe { new.add(i).write(fill) };
        }
        self.ptr = new;
        self.cap = cap;
    }

    fn get(&self, i: usize) -> Option<T> {
        // SAFETY: elements below cap are initialized.
        (i < self.cap).then(|| unsafe { *self.ptr.add(i) })
    }

    /// # Panics
    /// If `i` is not below the capacity.
    fn slot(&mut self, i: usize) -> &mut T {
        assert!(i < self.cap);
        // SAFETY: just checked, and elements below cap are initialized.
        unsafe { &mut *self.ptr.add(i) }
    }
}

const NO_ENTRY: u32 = u32::MAX;

/// The wait set: the descriptors to pass to poll(), in the order they were
/// first added, and for each descriptor number its index in that list.
struct WaitSet {
    entries: Buf<pollfd>,
    len: usize,
    index: Buf<u32>,
}

impl WaitSet {
    const fn new() -> Self {
        WaitSet { entries: Buf::new(), len: 0, index: Buf::new() }
    }

    fn clear(&mut self) {
        for i in 0..self.len {
            let fd = self.entries.get(i).unwrap().fd as usize;
            *self.index.slot(fd) = NO_ENTRY;
        }
        self.len = 0;
    }

    fn add(&mut self, fd: c_int, events: i16) {
        let Ok(n) = usize::try_from(fd) else {
            // select() would have written outside the set; poll() would
            // ignore it.  Either way it is a caller bug.
            server_panic(c"net_mplex: negative file descriptor")
        };
        self.index.reserve(n + 1, NO_ENTRY);
        let i = match *self.index.slot(n) {
            NO_ENTRY => {
                let i = self.len;
                self.entries.reserve(i + 1, pollfd { fd: -1, events: 0, revents: 0 });
                *self.entries.slot(i) = pollfd { fd, events: 0, revents: 0 };
                *self.index.slot(n) = i as u32;
                self.len += 1;
                i
            }
            i => i as usize,
        };
        self.entries.slot(i).events |= events;
    }

    /// Returns poll()'s result, or -1 with errno set to EBADF if any
    /// descriptor was not open.
    fn wait(&mut self, timeout_ms: c_int) -> c_int {
        let fds = if self.len == 0 { ptr::null_mut() } else { self.entries.ptr };
        // SAFETY: fds points to len initialized pollfds (or is null with len 0).
        let n = unsafe { libc::poll(fds, self.len as nfds_t, timeout_ms) };
        if n > 0 && (0..self.len).any(|i| self.entries.get(i).unwrap().revents & POLLNVAL != 0) {
            set_errno(libc::EBADF);
            return -1;
        }
        n
    }

    /// The events poll() reported for `fd` among those it was asked for.
    fn ready(&self, fd: c_int, wanted: i16, mask: i16) -> bool {
        let Some(i) = usize::try_from(fd).ok().and_then(|n| self.index.get(n)) else {
            return false;
        };
        if i == NO_ENTRY {
            return false;
        }
        let e = self.entries.get(i as usize).unwrap();
        e.events & wanted != 0 && e.revents & mask != 0
    }
}

fn set_errno(err: c_int) {
    // SAFETY: __errno_location returns this thread's errno.
    unsafe { *libc::__errno_location() = err };
}

static WAIT_SET: Global<WaitSet> = Global::new(WaitSet::new());

fn wait_set() -> &'static mut WaitSet {
    WAIT_SET.get_mut()
}

/// Seconds to poll()'s milliseconds, saturating instead of wrapping.
fn timeout_ms(seconds: c_uint) -> c_int {
    c_int::try_from(u64::from(seconds) * 1000).unwrap_or(c_int::MAX)
}

#[no_mangle]
pub extern "C" fn mplex_clear() {
    wait_set().clear();
}

#[no_mangle]
pub extern "C" fn mplex_add_reader(fd: c_int) {
    wait_set().add(fd, POLLIN);
}

#[no_mangle]
pub extern "C" fn mplex_add_writer(fd: c_int) {
    wait_set().add(fd, POLLOUT);
}

/// Returns -1 on error (errno set), 1 on timeout or interruption, and 0
/// when some descriptor is ready.
#[no_mangle]
pub extern "C" fn mplex_wait(timeout: c_uint) -> c_int {
    let n = wait_set().wait(timeout_ms(timeout));
    if n < 0 {
        // SAFETY: as in set_errno.
        if unsafe { *libc::__errno_location() } == libc::EINTR { 1 } else { -1 }
    } else {
        (n == 0) as c_int
    }
}

#[no_mangle]
pub extern "C" fn mplex_is_readable(fd: c_int) -> c_int {
    wait_set().ready(fd, POLLIN, READABLE) as c_int
}

#[no_mangle]
pub extern "C" fn mplex_is_writable(fd: c_int) -> c_int {
    wait_set().ready(fd, POLLOUT, WRITABLE) as c_int
}

#[cfg(test)]
mod tests {
    //! These share the one global wait set, so they run as a single test.

    use super::*;

    fn pipe() -> (c_int, c_int) {
        let mut fds = [0; 2];
        assert_eq!(unsafe { libc::pipe(fds.as_mut_ptr()) }, 0);
        (fds[0], fds[1])
    }

    fn close(fd: c_int) {
        unsafe { libc::close(fd) };
    }

    fn write_byte(fd: c_int) {
        assert_eq!(unsafe { libc::write(fd, b"x".as_ptr().cast(), 1) }, 1);
    }

    fn raise_fd_limit(want: libc::rlim_t) -> bool {
        let mut lim = libc::rlimit { rlim_cur: 0, rlim_max: 0 };
        unsafe { libc::getrlimit(libc::RLIMIT_NOFILE, &mut lim) };
        if lim.rlim_cur < want {
            lim.rlim_cur = want.min(lim.rlim_max);
            unsafe { libc::setrlimit(libc::RLIMIT_NOFILE, &lim) };
        }
        lim.rlim_cur >= want
    }

    #[test]
    fn multiplexer() {
        timeout_when_nothing_is_ready();
        readable_and_writable();
        hangup_is_readable();
        only_requested_directions_are_reported();
        descriptors_past_fd_setsize();
        closed_descriptor_fails_with_ebadf();
        timeouts_saturate();
    }

    fn timeout_when_nothing_is_ready() {
        let (r, w) = pipe();
        mplex_clear();
        mplex_add_reader(r);
        assert_eq!(mplex_wait(0), 1);
        assert_eq!(mplex_is_readable(r), 0);
        mplex_clear();
        assert_eq!(mplex_wait(0), 1, "an empty set times out");
        close(r);
        close(w);
    }

    fn readable_and_writable() {
        let (r, w) = pipe();
        write_byte(w);
        mplex_clear();
        mplex_add_reader(r);
        mplex_add_writer(w);
        mplex_add_reader(r);
        assert_eq!(mplex_wait(1), 0);
        assert_eq!(mplex_is_readable(r), 1);
        assert_eq!(mplex_is_writable(w), 1);
        assert_eq!(mplex_is_writable(r), 0);
        assert_eq!(mplex_is_readable(w), 0);
        assert_eq!(mplex_is_readable(12345), 0, "a descriptor never added");
        close(r);
        close(w);
    }

    fn hangup_is_readable() {
        let (r, w) = pipe();
        close(w);
        mplex_clear();
        mplex_add_reader(r);
        assert_eq!(mplex_wait(1), 0);
        assert_eq!(mplex_is_readable(r), 1, "EOF must wake the reader");
        close(r);
    }

    fn only_requested_directions_are_reported() {
        let (r, w) = pipe();
        write_byte(w);
        mplex_clear();
        mplex_add_writer(w);
        assert_eq!(mplex_wait(1), 0);
        mplex_clear();
        assert_eq!(mplex_is_writable(w), 0, "clear forgets earlier results");
        mplex_add_reader(w);
        mplex_add_writer(r);
        assert_eq!(mplex_wait(0), 1);
        close(r);
        close(w);
    }

    fn descriptors_past_fd_setsize() {
        if !raise_fd_limit(4096) {
            eprintln!("skipping: cannot raise RLIMIT_NOFILE to 4096");
            return;
        }
        let pipes: Vec<_> = (0..1100).map(|_| pipe()).collect();
        let (r, w) = *pipes.last().unwrap();
        assert!(r >= libc::FD_SETSIZE as c_int);
        write_byte(w);
        mplex_clear();
        for &(r, _) in &pipes {
            mplex_add_reader(r);
        }
        assert_eq!(mplex_wait(1), 0);
        assert_eq!(mplex_is_readable(r), 1);
        assert_eq!(pipes.iter().filter(|p| mplex_is_readable(p.0) == 1).count(), 1);
        for (r, w) in pipes {
            close(r);
            close(w);
        }
    }

    fn closed_descriptor_fails_with_ebadf() {
        let (r, w) = pipe();
        let (r2, w2) = pipe();
        write_byte(w2);
        close(r);
        mplex_clear();
        mplex_add_reader(r);
        mplex_add_reader(r2);
        assert_eq!(mplex_wait(1), -1);
        assert_eq!(unsafe { *libc::__errno_location() }, libc::EBADF);
        close(w);
        close(r2);
        close(w2);
    }

    fn timeouts_saturate() {
        assert_eq!(timeout_ms(5), 5000);
        assert_eq!(timeout_ms(c_uint::MAX), c_int::MAX);
    }
}
