static VTABLE_STUB: core::task::RawWakerVTable =
    core::task::RawWakerVTable::new(clone, wake, wake_by_ref, drop);

unsafe fn clone(data: *const ()) -> core::task::RawWaker {
    core::task::RawWaker::new(data, &VTABLE_STUB)
}

unsafe fn wake(_: *const ()) {}

unsafe fn wake_by_ref(_: *const ()) {}

unsafe fn drop(_: *const ()) {}

pub fn stub() -> core::task::Waker {
    let raw_waker = core::task::RawWaker::new(core::ptr::null(), &VTABLE_STUB);
    unsafe { core::task::Waker::from_raw(raw_waker) }
}
