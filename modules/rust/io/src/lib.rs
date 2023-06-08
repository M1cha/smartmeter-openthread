#![cfg_attr(not(feature = "std"), no_std)]

#[derive(Debug, snafu::Snafu)]
pub enum Error {
    #[snafu(display("Native error: {ret}"))]
    NativeUnsigned { ret: u32 },
    #[snafu(display("unexpected EOF"))]
    UnexpectedEof,
    #[snafu(display("unimplemented feature"))]
    Unimplemented,
    #[snafu(display("unknown error"))]
    Unknown,

    #[cfg(feature = "std")]
    #[snafu(display("std io error: {inner}"))]
    StdIoError { inner: std::io::Error },
}

pub trait AsyncRead {
    fn poll_read(
        self: core::pin::Pin<&mut Self>,
        cx: &mut core::task::Context<'_>,
        buf: &mut [u8],
    ) -> core::task::Poll<Result<usize, Error>>;
}

impl<T: ?Sized + AsyncRead + Unpin> AsyncRead for &mut T {
    fn poll_read(
        mut self: core::pin::Pin<&mut Self>,
        cx: &mut core::task::Context<'_>,
        buf: &mut [u8],
    ) -> core::task::Poll<Result<usize, Error>> {
        core::pin::Pin::new(&mut **self).poll_read(cx, buf)
    }
}

#[derive(Debug)]
#[must_use = "futures do nothing unless you `.await` or poll them"]
pub struct Read<'a, R: ?Sized> {
    reader: &'a mut R,
    buf: &'a mut [u8],
}

impl<R: ?Sized + Unpin> Unpin for Read<'_, R> {}

impl<'a, R: AsyncRead + ?Sized + Unpin> Read<'a, R> {
    fn new(reader: &'a mut R, buf: &'a mut [u8]) -> Self {
        Self { reader, buf }
    }
}

impl<R: AsyncRead + ?Sized + Unpin> core::future::Future for Read<'_, R> {
    type Output = Result<usize, Error>;

    fn poll(
        mut self: core::pin::Pin<&mut Self>,
        cx: &mut core::task::Context<'_>,
    ) -> core::task::Poll<Self::Output> {
        let this = &mut *self;
        core::pin::Pin::new(&mut this.reader).poll_read(cx, this.buf)
    }
}

/// Future for the [`read_exact`](AsyncReadExt::read_exact) method.
#[derive(Debug)]
#[must_use = "futures do nothing unless you `.await` or poll them"]
pub struct ReadExact<'a, R: ?Sized> {
    reader: &'a mut R,
    buf: &'a mut [u8],
}

impl<R: ?Sized + Unpin> Unpin for ReadExact<'_, R> {}

impl<'a, R: AsyncRead + ?Sized + Unpin> ReadExact<'a, R> {
    fn new(reader: &'a mut R, buf: &'a mut [u8]) -> Self {
        Self { reader, buf }
    }
}

impl<R: AsyncRead + ?Sized + Unpin> core::future::Future for ReadExact<'_, R> {
    type Output = Result<(), Error>;

    fn poll(
        mut self: core::pin::Pin<&mut Self>,
        cx: &mut core::task::Context<'_>,
    ) -> core::task::Poll<Self::Output> {
        let this = &mut *self;
        while !this.buf.is_empty() {
            let n = futures_util::ready!(
                core::pin::Pin::new(&mut this.reader).poll_read(cx, this.buf)
            )?;
            {
                let (_, rest) = core::mem::take(&mut this.buf).split_at_mut(n);
                this.buf = rest;
            }
            if n == 0 {
                return core::task::Poll::Ready(Err(Error::UnexpectedEof));
            }
        }
        core::task::Poll::Ready(Ok(()))
    }
}

#[cfg(feature = "std")]
mod std_impl {
    use super::*;

    /// use a [futures_util::io::AsyncRead] reader with this crate's reader
    pub struct FuturesUtilReader<R>(pub R);

    impl<R: futures_util::io::AsyncRead + Unpin> AsyncRead for FuturesUtilReader<R> {
        fn poll_read(
            mut self: core::pin::Pin<&mut Self>,
            cx: &mut core::task::Context<'_>,
            buf: &mut [u8],
        ) -> core::task::Poll<Result<usize, Error>> {
            core::pin::Pin::new(&mut self.0)
                .poll_read(cx, buf)
                .map_err(|inner| Error::StdIoError { inner })
        }
    }
}

#[cfg(feature = "std")]
pub use std_impl::*;

macro_rules! reader {
    ($name:ident, $ty:ty, $reader:ident) => {
        reader!($name, $ty, $reader, core::mem::size_of::<$ty>());
    };
    ($name:ident, $ty:ty, $reader:ident, $bytes:expr) => {
        #[pin_project::pin_project]
        #[must_use = "futures do nothing unless you `.await` or poll them"]
        pub struct $name<R> {
            #[pin]
            src: R,
            buf: [u8; $bytes],
            read: usize,
        }

        impl<R> $name<R> {
            pub(crate) fn new(src: R) -> Self {
                $name {
                    src,
                    buf: [0; $bytes],
                    read: 0,
                }
            }
        }

        impl<R> core::future::Future for $name<R>
        where
            R: AsyncRead,
        {
            type Output = Result<$ty, Error>;

            fn poll(
                self: core::pin::Pin<&mut Self>,
                cx: &mut core::task::Context<'_>,
            ) -> core::task::Poll<Self::Output> {
                let mut me = self.project();

                if *me.read == $bytes {
                    return core::task::Poll::Ready(Ok(<$ty>::$reader(*me.buf)));
                }

                while *me.read < $bytes {
                    *me.read += match me.src.as_mut().poll_read(cx, &mut me.buf[*me.read..]) {
                        core::task::Poll::Pending => return core::task::Poll::Pending,
                        core::task::Poll::Ready(Err(e)) => {
                            return core::task::Poll::Ready(Err(e.into()))
                        }
                        core::task::Poll::Ready(Ok(n)) => {
                            if n == 0 {
                                return core::task::Poll::Ready(Err(Error::UnexpectedEof.into()));
                            }

                            n
                        }
                    };
                }

                let num = <$ty>::$reader(*me.buf);
                core::task::Poll::Ready(Ok(num))
            }
        }
    };
}

macro_rules! read_impl {
    ($name:ident, $fut:ident) => {
        fn $name(&mut self) -> $fut<&mut Self>
        where
            Self: Unpin,
        {
            $fut::new(self)
        }
    };
}

reader!(ReadU8, u8, from_be_bytes);
reader!(ReadU16, u16, from_be_bytes);
reader!(ReadU32, u32, from_be_bytes);
reader!(ReadU64, u64, from_be_bytes);

reader!(ReadU8Le, u8, from_le_bytes);
reader!(ReadU16Le, u16, from_le_bytes);
reader!(ReadU32Le, u32, from_le_bytes);
reader!(ReadU64Le, u64, from_le_bytes);

reader!(ReadI8, i8, from_be_bytes);
reader!(ReadI16, i16, from_be_bytes);
reader!(ReadI32, i32, from_be_bytes);
reader!(ReadI64, i64, from_be_bytes);

reader!(ReadI8Le, i8, from_le_bytes);
reader!(ReadI16Le, i16, from_le_bytes);
reader!(ReadI32Le, i32, from_le_bytes);
reader!(ReadI64Le, i64, from_le_bytes);

pub trait AsyncReadExt: AsyncRead {
    read_impl!(read_u8, ReadU8);
    read_impl!(read_u16, ReadU16);
    read_impl!(read_u32, ReadU32);
    read_impl!(read_u64, ReadU64);

    read_impl!(read_u8_le, ReadU8Le);
    read_impl!(read_u16_le, ReadU16Le);
    read_impl!(read_u32_le, ReadU32Le);
    read_impl!(read_u64_le, ReadU64Le);

    read_impl!(read_i8, ReadI8);
    read_impl!(read_i16, ReadI16);
    read_impl!(read_i32, ReadI32);
    read_impl!(read_i64, ReadI64);

    read_impl!(read_i8_le, ReadI8Le);
    read_impl!(read_i16_le, ReadI16Le);
    read_impl!(read_i32_le, ReadI32Le);
    read_impl!(read_i64_le, ReadI64Le);

    fn read<'a>(&'a mut self, buf: &'a mut [u8]) -> Read<'a, Self>
    where
        Self: Unpin,
    {
        Read::new(self, buf)
    }

    fn read_exact<'a>(&'a mut self, buf: &'a mut [u8]) -> ReadExact<'a, Self>
    where
        Self: Unpin,
    {
        ReadExact::new(self, buf)
    }
}

impl<R: AsyncRead + ?Sized> AsyncReadExt for R {}

#[cfg(test)]
mod tests {
    use super::AsyncReadExt as _;

    #[test_log::test(tokio::test)]
    async fn basic() {
        let sampledata = [0xaau8, 0xbb, 0xcc, 0xdd];
        let mut reader = super::FuturesUtilReader(&sampledata[..]);
        assert_eq!(reader.read_u32().await.unwrap(), 0xaabbccdd);

        let mut reader = super::FuturesUtilReader(&sampledata[..]);
        assert_eq!(reader.read_u32_le().await.unwrap(), 0xddccbbaa);
    }
}
