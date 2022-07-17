#[derive(Debug)]
pub enum Error {
    /// received a different TLV than expected
    UnexpectedTlv {
        ty: crate::tlv::TlvType,
        len: usize,
    },
    /// TLV is shorter than required
    ShortTlvLength {
        len: usize,
    },
    /// received end marker that is not the last item in the current list
    MidMessageEndMarker {
        remaining: usize,
    },
    /// received multibyte TLV header with reserved flags
    MultibyteTlvReservedType {
        ty: u8,
    },
    /// TLV lengths field doesn't fit into our datatype for it
    TlvLengthTooBig,
    /// received end marker
    EndOfSmlMessage,
    /// received an unsupported TLV type
    UnsupportedTlvType {
        ty: u8,
    },
    /// received a different TLV type than expected
    ///
    /// this is returned when using the `next_*` APIs to read a value of a certain type
    UnexpectedValue,
    /// the TLV length doesn't match the type to be read
    UnsupportedLen {
        len: usize,
    },
    /// tried to read a value of a certain type but the end of the list was reached
    EndOfList,
    /// the frame or message checksum doesn't match
    ChecksumMismatch {
        rec: u16,
        calc: u16,
    },
    /// a SML choice has an unsupported tag
    UnsupportedTag {
        tag: u32,
    },
    /// tried to read into a buffer of the wrong size
    ///
    /// Some APIs like for reading `Octet String`s expect the correct buffer size.
    WrongBufferSize,
    /// `None`(a string of length 0) was received while trying to read a TLV header
    NoneTlv,
    /// Some APIs can't restrict usage at compile-time. Those return `CantParseTwice` on the second
    /// attempt
    CantParseTwice,

    Io(io::Error),
    TryFromIntError,
}

impl From<io::Error> for Error {
    fn from(source: io::Error) -> Self {
        Self::Io(source)
    }
}

impl From<core::num::TryFromIntError> for Error {
    fn from(_source: core::num::TryFromIntError) -> Self {
        Self::TryFromIntError
    }
}
