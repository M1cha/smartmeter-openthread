#![cfg_attr(not(feature = "std"), no_std)]
#![feature(generic_associated_types)]
#![feature(type_alias_impl_trait)]
#![allow(dead_code)]

mod capi;
#[cfg(feature = "logger")]
mod logger;
mod waker;

#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_panic: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}
