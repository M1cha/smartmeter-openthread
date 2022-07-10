//! Implements the whole C-API
//!
//! Ideally, this should be the only module with unsafe code in it.

use core::future::Future as _;

pub type ReadFn =
    extern "C" fn(buf: *mut core::ffi::c_void, max_length: usize, out_length: *mut usize) -> u32;

/// asynchronously write data to a C callback
pub struct Reader {
    function: ReadFn,
}

impl io::AsyncRead for Reader {
    fn poll_read(
        self: core::pin::Pin<&mut Self>,
        _cx: &mut core::task::Context<'_>,
        buf: &mut [u8],
    ) -> core::task::Poll<Result<usize, io::Error>> {
        let mut length = 0;

        let ret = (self.function)(
            buf.as_mut_ptr() as *mut core::ffi::c_void,
            buf.len(),
            &mut length,
        );
        if ret != 0 {
            return core::task::Poll::Ready(Err(io::Error::NativeUnsigned { ret }));
        }

        if length > 0 {
            core::task::Poll::Ready(Ok(length))
        } else {
            core::task::Poll::Pending
        }
    }
}

#[repr(C)]
#[derive(Debug)]
struct Value {
    scaler: i8,
    value: u64,
}

#[repr(C)]
#[derive(Debug)]
pub struct CallbackData {
    active_power: Value,
    active_energy: Value,
}

pub type MessageFn = extern "C" fn(*const CallbackData);

impl Value {
    pub fn new(scaler: i8, value: u64) -> Self {
        Self { scaler, value }
    }
}

/// handle SML messages and forward data to C
struct MessageCallback {
    function: MessageFn,
    active_power: Option<Value>,
    active_energy: Option<Value>,
}

impl MessageCallback {
    pub fn new(function: MessageFn) -> Self {
        Self {
            function,
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
        log::info!("finished, valid: {}, power={:?}, energy={:?}", valid, self.active_power, self.active_energy);

        if !valid {
            return;
        }

        match (self.active_power.take(), self.active_energy.take()) {
            (Some(active_power), Some(active_energy)) => {
                (self.function)(&CallbackData {
                    active_power,
                    active_energy,
                });
            }
            _ => return,
        }
    }
}

type SmlTaskFuture = impl core::future::Future<Output = Result<(), sml::Error>>;
fn sml_task_sized(mut reader: Reader, mut message_callback: MessageCallback) -> SmlTaskFuture {
    async move {
        sml::task(&mut reader, &mut message_callback).await?;

        Ok(())
    }
}

/// the main context pointer passed to us by C
pub struct Context {
    waker: core::task::Waker,
    f: SmlTaskFuture,
}

impl Context {
    fn new(reader: Reader, message_callback: MessageCallback) -> Self {
        Self {
            waker: crate::waker::stub(),
            f: sml_task_sized(reader, message_callback),
        }
    }
}

// workaround for cbindgen limitations: https://github.com/eqrion/cbindgen/issues/326#issuecomment-584288686
pub type ReadFnOpt = Option<
    extern "C" fn(buf: *mut core::ffi::c_void, max_length: usize, out_length: *mut usize) -> u32,
>;
pub type MessageFnOpt = Option<extern "C" fn(*const CallbackData)>;

/// initialize SML reader
///
/// none of the arguments must be NULL.
#[no_mangle]
pub extern "C" fn sml_init(
    out_context: *mut Context,
    context_len: usize,
    read_callback: ReadFnOpt,
    message_callback: MessageFnOpt,
) -> u32 {
    if out_context.is_null() {
        log::error!("out context is null");
        return 1;
    }
    let reader = match read_callback {
        Some(function) => Reader { function },
        None => {
            log::error!("read callback is null");
            return 2;
        }
    };
    let message_callback = match message_callback {
        Some(function) => MessageCallback::new(function),
        None => {
            log::error!("message callback is null");
            return 2;
        }
    };

    let min_size = core::mem::size_of::<Context>();
    if context_len < min_size {
        log::error!("callback is too small, min size: `{}`", min_size);
        return 2;
    }

    let context = Context::new(reader, message_callback);

    let out_context = out_context as *mut Context;
    // SAFETY: we verified the validity of the pointer and C also doens't ever move data
    unsafe { out_context.write(context) }

    0
}

/// process pending data
///
/// must be called as soon as the provided read callback can return data again.
#[no_mangle]
pub extern "C" fn sml_poll(mut context: core::ptr::NonNull<Context>) -> u32 {
    // SAFETY: We expect our callers to only pass initialized non-null pointers
    let context = unsafe { context.as_mut() };
    // SAFETY: C usually doesn't move memory around and we expect our callers
    //         to not do that
    let f = unsafe { core::pin::Pin::new_unchecked(&mut context.f) };

    let mut task_context = core::task::Context::from_waker(&context.waker);
    match f.poll(&mut task_context) {
        core::task::Poll::Ready(res) => {
            log::error!("task ended with: {:?}", res);
            1
        }
        core::task::Poll::Pending => 0,
    }
}

/// return the buffer size required for the sml context
#[no_mangle]
pub extern "C" fn sml_ctxsz() -> usize {
    core::mem::size_of::<Context>()
}

#[cfg(test)]
mod tests {
    #[test_log::test]
    fn basic() {
        eprintln!("context size = {}", super::sml_ctxsz());

        extern "C" fn read_callback(
            _buf: *mut core::ffi::c_void,
            _max_length: usize,
            _out_length: *mut usize,
        ) -> u32 {
            0
        }

        extern "C" fn message_callback(_scaler: i8, _value: i64) -> u32 {
            0
        }

        let mut context = core::mem::MaybeUninit::<super::Context>::uninit();
        assert_eq!(
            super::sml_init(
                context.as_mut_ptr(),
                core::mem::size_of::<super::Context>(),
                Some(read_callback),
                Some(message_callback),
            ),
            0
        );
        let context = unsafe { context.assume_init_mut() };

        super::sml_poll(context.into());
    }
}
