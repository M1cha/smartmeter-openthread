#![cfg_attr(not(feature = "std"), no_std)]
#![feature(generic_associated_types)]
#![feature(type_alias_impl_trait)]
//#![allow(dead_code)]

mod capi;
#[cfg(feature = "logger")]
mod logger;
mod waker;

/// this is not called when `panic_immediate_abort` is enabled
#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_panic: &core::panic::PanicInfo<'_>) -> ! {
    loop {}
}

#[repr(C)]
#[derive(Debug)]
pub struct Value {
    scaler: i8,
    value: u64,
}

impl Value {
    pub fn new(scaler: i8, value: u64) -> Self {
        Self { scaler, value }
    }
}

#[repr(C)]
#[derive(Debug)]
pub struct CallbackData {
    active_power: Value,
    active_energy: Value,
}

pub type MessageFn = extern "C" fn(*mut core::ffi::c_void, *const CallbackData);

/// handle SML messages and forward data to C
struct MessageCallback {
    function: MessageFn,
    user_context: *mut core::ffi::c_void,
    active_power: Option<Value>,
    active_energy: Option<Value>,
}

impl MessageCallback {
    pub fn new(function: MessageFn, user_context: *mut core::ffi::c_void) -> Self {
        Self {
            function,
            user_context,
            active_power: None,
            active_energy: None,
        }
    }
}

impl<R: io::AsyncRead + Unpin> sml::Callback<R> for MessageCallback {
    type Fut<'a> = impl core::future::Future<Output = Result<(), sml::Error>> where R:'a;

    fn frame_start(&mut self) {
        self.active_power = None;
        self.active_energy = None;
    }

    fn message_received<'a>(
        &'a mut self,
        mut body: sml::types::MessageBody<'a, R>,
    ) -> Self::Fut<'a> {
        const OBIS_ACTIVE_ENERGY: &[u8] = &[0x01, 0x00, 0x01, 0x08, 0x00, 0xFF];
        const OBIS_ACTIVE_POWER: &[u8] = &[0x01, 0x00, 0x10, 0x07, 0x00, 0xFF];

        async move {
            match body.read().await? {
                sml::types::MessageBodyEnum::GetListResponse(r) => {
                    let mut field = r.val_list().await?;
                    let mut list = field.parse().await?;

                    let mut buf = [0; 7];
                    while let Some(entry) = list.next().await? {
                        let mut field = entry.obj_name().await?;
                        let mut obj_name = field.parse().await?;

                        if obj_name.len() > buf.len() {
                            continue;
                        }

                        let buf = &mut buf[..obj_name.len()];
                        obj_name.read(buf).await?;
                        drop(obj_name);
                        let entry = field.finish().await?;

                        let (entry, scaler) = entry.scaler().await?;
                        let scaler = scaler.unwrap_or(0);
                        log::debug!("scaler={}", scaler);

                        let mut field = entry.value().await?;
                        let value = field.parse().await?.read().await?;
                        match &*buf {
                            OBIS_ACTIVE_POWER => {
                                let value: u64 = value.into_i128_relaxed()?.try_into()?;
                                log::debug!("active_power={}", value);

                                if self.active_power.is_some() {
                                    return Err(sml::Error::UnexpectedValue);
                                }

                                self.active_power = Some(Value::new(scaler, value));
                            }

                            OBIS_ACTIVE_ENERGY => {
                                let value: u64 = value.into_i128_relaxed()?.try_into()?;
                                log::debug!("active_energy={}", value);

                                if self.active_energy.is_some() {
                                    return Err(sml::Error::UnexpectedValue);
                                }

                                self.active_energy = Some(Value::new(scaler, value));
                            }
                            _ => continue,
                        }
                    }
                }
                _ => return Ok(()),
            }

            Ok(())
        }
    }

    fn frame_finished(&mut self, valid: bool) {
        log::info!(
            "finished, valid: {}, power={:?}, energy={:?}",
            valid,
            self.active_power,
            self.active_energy
        );

        if !valid {
            return;
        }

        if let (Some(active_power), Some(active_energy)) =
            (self.active_power.take(), self.active_energy.take())
        {
            (self.function)(
                self.user_context,
                &CallbackData {
                    active_power,
                    active_energy,
                },
            );
        }
    }
}
