static VTABLE_STUB: core::task::RawWakerVTable =
    core::task::RawWakerVTable::new(clone, wake, wake_by_ref, drop);

unsafe fn clone(data: *const ()) -> core::task::RawWaker {
    core::task::RawWaker::new(data, &VTABLE_STUB)
}

unsafe fn wake(_: *const ()) {}

unsafe fn wake_by_ref(_: *const ()) {}

unsafe fn drop(_: *const ()) {}

/// create a waker that doesn't do anything
///
/// this can be useful in limited environments where it's known by the API user
/// what the async code is waiting for and when to call the poll function again.
pub fn stub() -> core::task::Waker {
    let raw_waker = core::task::RawWaker::new(core::ptr::null(), &VTABLE_STUB);
    unsafe { core::task::Waker::from_raw(raw_waker) }
}
