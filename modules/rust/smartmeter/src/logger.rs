//! a logger that writes data via a C callback

use core::fmt::Write as _;

static STATE: core::sync::atomic::AtomicUsize = core::sync::atomic::AtomicUsize::new(0);
static mut SINK_CALLBACK: SinkCallback = nop_sink;
static LOGGER: SimpleLogger = SimpleLogger {};

const UNINITIALIZED: usize = 0;
const INITIALIZED: usize = 1;

#[repr(C)]
#[derive(Copy, Clone)]
pub enum CLogLevel {
    Error,
    Warn,
    Info,
    Debug,
    Trace,
}

type SinkCallback =
    extern "C" fn(level: CLogLevel, buf: *const core::ffi::c_void, len: usize) -> u32;
type SinkCallbackOpt =
    Option<extern "C" fn(level: CLogLevel, buf: *const core::ffi::c_void, len: usize) -> u32>;

/// returns the current sink or an empty one if there is none
pub fn sink() -> SinkCallback {
    if STATE.load(core::sync::atomic::Ordering::SeqCst) != INITIALIZED {
        nop_sink
    } else {
        unsafe { SINK_CALLBACK }
    }
}

/// implements [core::fmt::Write] via a C sink callback
struct LogWriter {
    level: CLogLevel,
    sink: SinkCallback,
}

impl LogWriter {
    pub fn new(level: log::Level) -> Self {
        let level = match level {
            log::Level::Error => CLogLevel::Error,
            log::Level::Warn => CLogLevel::Warn,
            log::Level::Info => CLogLevel::Info,
            log::Level::Debug => CLogLevel::Debug,
            log::Level::Trace => CLogLevel::Trace,
        };

        Self {
            level,
            sink: sink(),
        }
    }

    /// tell the C side that the current log entry has ended
    pub fn finish_record(&mut self) {
        let _ = (self.sink)(self.level, core::ptr::null(), 0);
    }
}

impl core::fmt::Write for LogWriter {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        match (self.sink)(self.level, s.as_ptr() as *const core::ffi::c_void, s.len()) {
            0 => Ok(()),
            _ => Err(core::fmt::Error),
        }
    }
}

/// implements [log::Log] with a [LogWriter]
struct SimpleLogger;

impl log::Log for SimpleLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= log::Level::Debug
    }

    fn log(&self, record: &log::Record) {
        if self.enabled(record.metadata()) {
            let mut writer = LogWriter::new(record.level());
            let _ = write!(&mut writer, "{}: {}", record.target(), record.args());
            writer.finish_record();
        }
    }

    fn flush(&self) {}
}

/// A sink that doesn't to anything
///
/// This is useful so we don't have to do any null-pointer checks since we'll
/// always have a sink.
extern "C" fn nop_sink(_level: CLogLevel, _s: *const core::ffi::c_void, _len: usize) -> u32 {
    0
}

/// Initialize SML logger
///
/// - should only be called once
/// - `sink` must not be NULL
/// - must not be called while running other functions of this library in
///   parallel - e.g. on threads or during interrupts.
#[no_mangle]
pub unsafe extern "C" fn smr_init_logger(sink: SinkCallbackOpt) -> u32 {
    let sink = match sink {
        Some(v) => v,
        None => {
            return 1;
        }
    };

    match STATE.load(core::sync::atomic::Ordering::SeqCst) {
        UNINITIALIZED => {
            SINK_CALLBACK = sink;
            STATE.store(INITIALIZED, core::sync::atomic::Ordering::SeqCst);
        }
        _ => return 2,
    }

    match log::set_logger_racy(&LOGGER).map(|()| log::set_max_level(log::LevelFilter::Debug)) {
        Err(_) => 3,
        Ok(_) => 0,
    }
}
