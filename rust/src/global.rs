//! Module-level state for the server's main thread.

use core::cell::UnsafeCell;

/// A value that only the server's main thread touches.  The network layer
/// and the other ported modules run on that one thread, so there is no
/// locking.
pub struct Global<T>(UnsafeCell<T>);

// SAFETY: only the server's main thread uses these values.
unsafe impl<T> Sync for Global<T> {}

impl<T> Global<T> {
    pub const fn new(value: T) -> Self {
        Global(UnsafeCell::new(value))
    }

    /// The value itself, to change in place.
    ///
    /// Callers must not hold the reference across a call that might take
    /// it again.
    #[allow(clippy::mut_from_ref)]
    pub fn get_mut(&self) -> &mut T {
        // SAFETY: single-threaded, and see the rule above.
        unsafe { &mut *self.0.get() }
    }
}
