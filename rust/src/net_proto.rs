//! BSD sockets TCP/IP: a port of net_bsd_tcp.c and net_tcp.c, which
//! net_proto.c includes when NETWORK_PROTOCOL is NP_TCP and NETWORK_STYLE
//! is NS_BSD.
//!
//! This is the server's edge with the operating system's network stack:
//! command-line options, listening sockets, accepting connections and
//! open_network_connection().  Host names come from name_lookup.c, which
//! stays in C.
//!
//! One behavior differs on purpose.  The C proto_open_connection() bounds
//! connect() with a SIGALRM timer and longjmp()s out of the signal handler
//! when it fires; that cannot cross Rust frames, and it is fragile in C
//! too.  The port connects without blocking and waits for completion with
//! poll(), so the outbound_connect_timeout is exact rather than rounded to
//! the clock's second, and a timeout of 0 means "only if it connects
//! immediately" instead of always failing.

use core::ffi::{c_char, c_int, CStr};
use core::fmt::{self, Write};
use core::mem::{size_of, zeroed};
use core::ptr;

use libc::{in_addr_t, sockaddr, sockaddr_in, socklen_t, AF_INET, INADDR_ANY, SOCK_STREAM};

use crate::ffi::{
    error, initialize_name_lookup, log_perror, lookup_addr_from_name,
    lookup_name_from_addr, oklog, proto, proto_accept_error, server_listener,
    server_listener_int_option, var_type, Var, DEFAULT_PORT, MOO_RS_BSD_TCP,
    MOO_RS_OUTBOUND_NETWORK_DEFAULT, MOO_RS_OUTBOUND_NETWORK_SUPPORTED,
};
use crate::global::Global;

const _: () = assert!(MOO_RS_BSD_TCP == 1, "net_proto ports only the BSD TCP/IP protocol");

const OUTBOUND_SUPPORTED: bool = MOO_RS_OUTBOUND_NETWORK_SUPPORTED != 0;

/// The local address to bind listeners and outbound connections to, from
/// the -a option; INADDR_ANY means any.
static BIND_LOCAL_IP: Global<in_addr_t> = Global::new(INADDR_ANY);

/// A fixed buffer for the names the C side borrows until the next call.
/// Text that does not fit is cut off.
struct NameBuf<const N: usize> {
    buf: [u8; N],
    len: usize,
}

impl<const N: usize> NameBuf<N> {
    const fn new() -> Self {
        NameBuf { buf: [0; N], len: 0 }
    }

    /// Set the buffer to `prefix` (bytes from a C string, copied as they
    /// are) followed by `rest`, and return it as a C string.
    fn set(&mut self, prefix: &[u8], rest: fmt::Arguments) -> *const c_char {
        self.len = 0;
        self.push(prefix);
        let _ = self.write_fmt(rest);
        self.buf[self.len] = 0;
        self.buf.as_ptr().cast()
    }

    fn push(&mut self, bytes: &[u8]) {
        let n = bytes.len().min(N - 1 - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&bytes[..n]);
        self.len += n;
    }
}

impl<const N: usize> Write for NameBuf<N> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        self.push(s.as_bytes());
        Ok(())
    }
}

/// The bytes of a C string from the server.
///
/// # Safety
/// `s` must be NUL-terminated and outlive the result.
unsafe fn bytes<'a>(s: *const c_char) -> &'a [u8] {
    // SAFETY: per the contract above.
    unsafe { CStr::from_ptr(s) }.to_bytes()
}

fn errno() -> c_int {
    // SAFETY: __errno_location returns this thread's errno.
    unsafe { *libc::__errno_location() }
}

fn set_errno(err: c_int) {
    // SAFETY: as in errno().
    unsafe { *libc::__errno_location() = err };
}

fn perror(what: &CStr) {
    // SAFETY: a NUL-terminated message; log_perror reads errno.
    unsafe { log_perror(what.as_ptr()) };
}

fn close(fd: c_int) {
    // SAFETY: closing a descriptor this module opened.
    unsafe { libc::close(fd) };
}

fn int_option(sl: server_listener, name: &CStr, default: c_int) -> c_int {
    // SAFETY: a listener handle from the server and a NUL-terminated name.
    // The C code keeps the result in an int too.
    unsafe { server_listener_int_option(sl, name.as_ptr(), default.into()) as c_int }
}

fn sockaddr_in_for(addr: in_addr_t, port: u16) -> sockaddr_in {
    // SAFETY: sockaddr_in is plain data; all zeroes is a valid value.
    let mut a: sockaddr_in = unsafe { zeroed() };
    a.sin_family = AF_INET as libc::sa_family_t;
    a.sin_addr.s_addr = addr;
    a.sin_port = port.to_be();
    a
}

const SOCKADDR_IN_LEN: socklen_t = size_of::<sockaddr_in>() as socklen_t;

#[no_mangle]
pub extern "C" fn proto_name() -> *const c_char {
    c"BSD/TCP".as_ptr()
}

#[no_mangle]
pub extern "C" fn proto_usage_string() -> *const c_char {
    c"[+O|-O] [+N|-N] [-a ip_address] [[-p] port]".as_ptr()
}

/// The argument at `i`, which must be in range.
unsafe fn arg(argv: *mut *mut c_char, i: c_int) -> &'static CStr {
    // SAFETY: the caller's argv holds argc NUL-terminated strings.
    unsafe { CStr::from_ptr(*argv.add(i as usize)) }
}

/// Parse the protocol's command-line arguments.  Returns false on a
/// malformed command line.
///
/// # Safety
/// `argv` must hold `argc` NUL-terminated strings.
unsafe fn tcp_arguments(
    proto: &mut proto,
    argc: c_int,
    argv: *mut *mut c_char,
    port: &mut c_int,
    name_lookup: &mut c_int,
) -> bool {
    let mut seen_port = false;
    let mut i = 0;
    while i < argc {
        // SAFETY: i < argc.
        let a = unsafe { arg(argv, i) }.to_bytes();
        match a {
            [sign @ (b'-' | b'+'), b'O'] => {
                if OUTBOUND_SUPPORTED {
                    proto.can_connect_outbound = (*sign == b'+') as c_int;
                } else if *sign == b'+' {
                    // SAFETY: format strings and arguments match.
                    unsafe {
                        libc::fprintf(stderr, c"Outbound network not supported.\n".as_ptr());
                        oklog(
                            c"CMDLINE: *** Ignoring %s (outbound network not supported)\n".as_ptr(),
                            *argv.add(i as usize),
                        );
                    }
                }
            }
            [sign @ (b'-' | b'+'), b'N'] => *name_lookup = (*sign == b'+') as c_int,
            b"-a" => {
                if i + 1 >= argc {
                    return false;
                }
                i += 1;
                // SAFETY: i < argc.
                let ip = unsafe { *argv.add(i as usize) };
                // SAFETY: a NUL-terminated string.
                let addr = unsafe { inet_addr(ip) };
                if addr == libc::INADDR_NONE {
                    return false;
                }
                BIND_LOCAL_IP.set(addr);
                // SAFETY: format strings and arguments match.
                unsafe { oklog(c"CMDLINE: Source address restricted to %s\n".as_ptr(), ip) };
            }
            _ => {
                // A second port argument is an error.
                if seen_port {
                    return false;
                }
                if a == b"-p" {
                    if i + 1 >= argc {
                        return false;
                    }
                    i += 1;
                }
                // Like the C code: strtoul()'s syntax, truncated to an int.
                let mut end: *mut c_char = ptr::null_mut();
                // SAFETY: i < argc; strtoul reads a NUL-terminated string.
                *port = unsafe { libc::strtoul(*argv.add(i as usize), &mut end, 10) } as c_int;
                seen_port = true;
                // SAFETY: strtoul leaves end inside the string.
                if unsafe { *end } != 0 {
                    return false;
                }
                // SAFETY: format strings and arguments match.
                unsafe { oklog(c"CMDLINE: Initial port = %d\n".as_ptr(), *port) };
            }
        }
        i += 1;
    }
    // SAFETY: format strings and arguments match.
    unsafe {
        if OUTBOUND_SUPPORTED {
            oklog(
                c"CMDLINE: Outbound network connections %s.\n".as_ptr(),
                if proto.can_connect_outbound != 0 { c"enabled" } else { c"disabled" }.as_ptr(),
            );
        }
        oklog(
            c"CMDLINE: DNS name lookups %s.\n".as_ptr(),
            if *name_lookup != 0 { c"enabled" } else { c"disabled; numeric addresses only" }
                .as_ptr(),
        );
    }
    true
}

// C library declarations the libc crate lacks.
extern "C" {
    static mut stderr: *mut libc::FILE;
    fn inet_addr(cp: *const c_char) -> in_addr_t;
}

/// # Safety
/// `proto` and `desc` must be valid, and `argv` must hold `argc`
/// NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn proto_initialize(
    proto: *mut proto,
    desc: *mut Var,
    argc: c_int,
    argv: *mut *mut c_char,
) -> c_int {
    // SAFETY: the caller passes a valid proto.
    let proto = unsafe { &mut *proto };
    proto.can_connect_outbound = MOO_RS_OUTBOUND_NETWORK_DEFAULT as c_int;
    let mut port = DEFAULT_PORT as c_int;
    let mut name_lookup = 1;
    proto.pocket_size = 1;
    proto.believe_eof = 1;
    proto.eol_out_string = c"\r\n".as_ptr();

    // SAFETY: passed on from our caller.
    if !unsafe { tcp_arguments(proto, argc, argv, &mut port, &mut name_lookup) } {
        return 0;
    }
    // SAFETY: plain C call.
    unsafe { initialize_name_lookup(name_lookup) };
    // SAFETY: the caller passes a valid Var to fill in.
    unsafe {
        (*desc).type_ = var_type::TYPE_INT;
        (*desc).v.num = port.into();
    }
    1
}

static LISTENER_NAME: Global<NameBuf<32>> = Global::new(NameBuf::new());

/// # Safety
/// `fd`, `canon` and `name` must be valid for writing.
#[no_mangle]
pub unsafe extern "C" fn proto_make_listener(
    desc: Var,
    fd: *mut c_int,
    canon: *mut Var,
    name: *mut *const c_char,
) -> error::Type {
    if desc.type_ != var_type::TYPE_INT {
        return error::E_TYPE;
    }
    // SAFETY: the type says which union member is set.
    let num = unsafe { desc.v.num };
    let Ok(port) = u16::try_from(num) else {
        return error::E_INVARG; // would wrap onto another port
    };

    // SAFETY: plain socket calls on a descriptor we own.
    let s = unsafe { libc::socket(AF_INET, SOCK_STREAM, 0) };
    if s < 0 {
        perror(c"Creating listening socket");
        return error::E_QUOTA;
    }
    let option: c_int = 1;
    // SAFETY: option is a c_int of the size passed.
    if unsafe {
        libc::setsockopt(
            s,
            libc::SOL_SOCKET,
            libc::SO_REUSEADDR,
            (&option as *const c_int).cast(),
            size_of::<c_int>() as socklen_t,
        )
    } < 0
    {
        perror(c"Setting listening socket options");
        close(s);
        return error::E_QUOTA;
    }
    let mut address = sockaddr_in_for(BIND_LOCAL_IP.get(), port);
    // SAFETY: address is a sockaddr_in of the length passed.
    if unsafe { libc::bind(s, (&address as *const sockaddr_in).cast(), SOCKADDR_IN_LEN) } < 0 {
        perror(c"Binding listening socket");
        let e = if errno() == libc::EACCES { error::E_PERM } else { error::E_QUOTA };
        close(s);
        return e;
    }
    let canon_port = if port == 0 {
        let mut length = SOCKADDR_IN_LEN;
        // SAFETY: address has room for length bytes.
        if unsafe { libc::getsockname(s, (&mut address as *mut sockaddr_in).cast(), &mut length) }
            < 0
        {
            perror(c"Discovering local port number");
            close(s);
            return error::E_QUOTA;
        }
        u16::from_be(address.sin_port)
    } else {
        port
    };
    // SAFETY: the caller's out-parameters are valid.  An integer Var needs
    // no reference counting, so this is var_ref(desc) when port != 0.
    unsafe {
        (*canon).type_ = var_type::TYPE_INT;
        (*canon).v.num = canon_port.into();
        *name = LISTENER_NAME.get_mut().set(b"", format_args!("port {canon_port}"));
        *fd = s;
    }
    error::E_NONE
}

#[no_mangle]
pub extern "C" fn proto_listen(fd: c_int) -> c_int {
    // SAFETY: plain C call.
    unsafe { libc::listen(fd, 5) };
    1
}

/// address_lookup_timeout replaces name_lookup_timeout for inbound
/// connections; the older option still applies when the newer is unset.
fn address_lookup_timeout(sl: server_listener) -> c_int {
    let mut timeout = int_option(sl, c"address_lookup_timeout", -2);
    if timeout == -2 {
        timeout = int_option(sl, c"name_lookup_timeout", 5);
    }
    timeout.max(0)
}

static PEER_NAME: Global<NameBuf<1024>> = Global::new(NameBuf::new());

/// # Safety
/// The out-parameters must be valid for writing.
#[no_mangle]
pub unsafe extern "C" fn proto_accept_connection(
    listener_fd: c_int,
    sl: server_listener,
    read_fd: *mut c_int,
    write_fd: *mut c_int,
    name: *mut *const c_char,
) -> proto_accept_error {
    let timeout = address_lookup_timeout(sl);
    // SAFETY: sockaddr_in is plain data.
    let mut address: sockaddr_in = unsafe { zeroed() };
    let mut length = SOCKADDR_IN_LEN;
    // SAFETY: address has room for length bytes.
    let fd = unsafe {
        libc::accept(listener_fd, (&mut address as *mut sockaddr_in).cast::<sockaddr>(), &mut length)
    };
    if fd < 0 {
        if errno() == libc::EMFILE {
            return proto_accept_error::PA_FULL;
        }
        perror(c"Accepting new network connection");
        return proto_accept_error::PA_OTHER;
    }
    // SAFETY: lookup_name_from_addr reads the address and returns a string
    // that stays valid until the next lookup.
    let host = unsafe { lookup_name_from_addr(&mut address, timeout as u32) };
    let port = u16::from_be(address.sin_port);
    // SAFETY: the caller's out-parameters are valid.
    unsafe {
        *read_fd = fd;
        *write_fd = fd;
        *name = PEER_NAME.get_mut().set(bytes(host), format_args!(", port {port}"));
    }
    proto_accept_error::PA_OKAY
}

#[no_mangle]
pub extern "C" fn proto_close_connection(read_fd: c_int, _write_fd: c_int) {
    // read_fd and write_fd are the same, so we only need to deal with one.
    close(read_fd);
}

#[no_mangle]
pub extern "C" fn proto_close_listener(fd: c_int) {
    close(fd);
}

/// Check open_network_connection()'s arguments: {host_name, port}.
///
/// # Safety
/// `arglist` must be a valid MOO list.
unsafe fn open_connection_arguments(arglist: &Var) -> Result<(*const c_char, u16), error::Type> {
    // SAFETY: a list's element 0 holds its length, then come the elements.
    let list = unsafe { arglist.v.list };
    let item = |i: usize| unsafe { &*list.add(i) };
    if unsafe { item(0).v.num } != 2 {
        return Err(error::E_ARGS);
    }
    if item(1).type_ != var_type::TYPE_STR || item(2).type_ != var_type::TYPE_INT {
        return Err(error::E_TYPE);
    }
    // SAFETY: types checked just above.
    let (host, port) = unsafe { (item(1).v.str_, item(2).v.num) };
    match u16::try_from(port) {
        Ok(port) => Ok((host, port)),
        Err(_) => Err(error::E_INVARG), // would wrap onto another port
    }
}

/// Connect `s` to `addr`, giving up after `timeout` seconds.  On failure
/// returns false with errno set, ETIMEDOUT for a timeout.
fn connect_with_timeout(s: c_int, addr: &sockaddr_in, timeout: c_int) -> bool {
    // SAFETY: plain fcntl calls on a descriptor we own.
    let flags = unsafe { libc::fcntl(s, libc::F_GETFL) };
    if flags < 0 || unsafe { libc::fcntl(s, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
        return false;
    }
    // SAFETY: addr is a sockaddr_in of the length passed.
    let mut ok = unsafe { libc::connect(s, (addr as *const sockaddr_in).cast(), SOCKADDR_IN_LEN) }
        == 0;
    if !ok && errno() == libc::EINPROGRESS {
        ok = wait_for_connect(s, timeout);
    }
    let err = errno();
    // SAFETY: as above.  The network layer sets its own flags later.
    unsafe { libc::fcntl(s, libc::F_SETFL, flags) };
    set_errno(err);
    ok
}

fn wait_for_connect(s: c_int, timeout: c_int) -> bool {
    let deadline_ms = i64::from(timeout.max(0)) * 1000;
    let start = now_ms();
    loop {
        let left = (deadline_ms - (now_ms() - start)).max(0);
        let mut pfd = libc::pollfd { fd: s, events: libc::POLLOUT, revents: 0 };
        // SAFETY: one pollfd.
        let n = unsafe { libc::poll(&mut pfd, 1, left.min(c_int::MAX.into()) as c_int) };
        if n < 0 && errno() == libc::EINTR {
            continue;
        }
        if n < 0 {
            return false;
        }
        if n == 0 {
            set_errno(libc::ETIMEDOUT);
            return false;
        }
        let mut err: c_int = 0;
        let mut len = size_of::<c_int>() as socklen_t;
        // SAFETY: err is a c_int of the length passed.
        if unsafe {
            libc::getsockopt(s, libc::SOL_SOCKET, libc::SO_ERROR, (&mut err as *mut c_int).cast(), &mut len)
        } < 0
        {
            return false;
        }
        if err != 0 {
            set_errno(err);
            return false;
        }
        return true;
    }
}

fn now_ms() -> i64 {
    // SAFETY: timespec is plain data, filled in by clock_gettime.
    let mut ts: libc::timespec = unsafe { zeroed() };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec * 1000 + ts.tv_nsec / 1_000_000
}

static LOCAL_NAME: Global<NameBuf<32>> = Global::new(NameBuf::new());
static REMOTE_NAME: Global<NameBuf<1024>> = Global::new(NameBuf::new());

/// open_network_connection(): connect to a host and port.
///
/// # Safety
/// `arglist` must be a valid MOO list and the out-parameters valid for
/// writing.
#[no_mangle]
pub unsafe extern "C" fn proto_open_connection(
    arglist: Var,
    sl: server_listener,
    read_fd: *mut c_int,
    write_fd: *mut c_int,
    local_name: *mut *const c_char,
    remote_name: *mut *const c_char,
) -> error::Type {
    // SAFETY: passed on from our caller.
    let (host_name, port) = match unsafe { open_connection_arguments(&arglist) } {
        Ok(v) => v,
        Err(e) => return e,
    };
    let timeout = int_option(sl, c"name_lookup_timeout", 5);

    // SAFETY: host_name is a NUL-terminated MOO string.
    let host = unsafe { lookup_addr_from_name(host_name, timeout as u32) };
    if host == 0 {
        return error::E_INVARG;
    }
    let mut addr = sockaddr_in_for(host, port);

    // SAFETY: plain C call.
    let s = unsafe { libc::socket(AF_INET, SOCK_STREAM, 0) };
    if s < 0 {
        if errno() != libc::EMFILE {
            perror(c"Making socket in proto_open_connection");
        }
        return error::E_QUOTA;
    }

    let local_ip = BIND_LOCAL_IP.get();
    if local_ip != INADDR_ANY {
        let local = sockaddr_in_for(local_ip, 0);
        // In theory, if the original listen() succeeded, then this should
        // too, but who knows, really?
        // SAFETY: local is a sockaddr_in of the length passed.
        if unsafe { libc::bind(s, (&local as *const sockaddr_in).cast(), SOCKADDR_IN_LEN) } < 0 {
            perror(c"Binding local address in proto_open_connection");
            let e = if errno() == libc::EACCES { error::E_PERM } else { error::E_QUOTA };
            close(s);
            return e;
        }
    }

    let connect_timeout = int_option(sl, c"outbound_connect_timeout", 5);
    if !connect_with_timeout(s, &addr, connect_timeout) {
        let err = errno();
        close(s);
        if matches!(err, libc::EADDRNOTAVAIL | libc::ECONNREFUSED | libc::ENETUNREACH | libc::ETIMEDOUT) {
            return error::E_INVARG;
        }
        set_errno(err);
        perror(c"Connecting in proto_open_connection");
        return error::E_QUOTA;
    }
    let mut length = SOCKADDR_IN_LEN;
    // SAFETY: addr has room for length bytes.
    if unsafe { libc::getsockname(s, (&mut addr as *mut sockaddr_in).cast(), &mut length) } < 0 {
        let err = errno();
        close(s);
        set_errno(err);
        perror(c"Getting local name in proto_open_connection");
        return error::E_QUOTA;
    }
    let local_port = u16::from_be(addr.sin_port);
    // SAFETY: the caller's out-parameters are valid.
    unsafe {
        *read_fd = s;
        *write_fd = s;
        *local_name = LOCAL_NAME.get_mut().set(b"", format_args!("port {local_port}"));
        *remote_name = REMOTE_NAME.get_mut().set(bytes(host_name), format_args!(", port {port}"));
    }
    error::E_NONE
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text(p: *const c_char) -> &'static str {
        unsafe { CStr::from_ptr(p) }.to_str().unwrap()
    }

    #[test]
    fn names_are_formatted_and_cut_to_fit() {
        let mut b = NameBuf::<16>::new();
        assert_eq!(text(b.set(b"", format_args!("port {}", 7777))), "port 7777");
        assert_eq!(text(b.set(b"host", format_args!(", port {}", 1))), "host, port 1");
        assert_eq!(text(b.set(b"a.long.host.name", format_args!(", port {}", 1))), "a.long.host.nam");
    }
}
