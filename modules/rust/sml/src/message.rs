/// wraps an [io::AsyncRead]er and computes a checksum
pub struct CheckingReader<'a, R> {
    inner: &'a mut R,
    digest: crc::Digest<'static, u16>,
}

impl<'a, R> CheckingReader<'a, R> {
    pub fn new(reader: &'a mut R) -> Self {
        Self {
            inner: reader,
            digest: crate::CRC_INSTANCE.digest(),
        }
    }

    /// resets the digest and returns the checksum
    pub fn finalize(&mut self) -> u16 {
        let mut digest = crate::CRC_INSTANCE.digest();
        core::mem::swap(&mut digest, &mut self.digest);

        digest.finalize()
    }

    pub fn reset(&mut self) {
        self.digest = crate::CRC_INSTANCE.digest();
    }
}

impl<'a, R: crate::ReaderEnded> crate::ReaderEnded for CheckingReader<'a, R> {
    fn has_ended(&self) -> bool {
        self.inner.has_ended()
    }
}

impl<'a, R: io::AsyncRead + Unpin> io::AsyncRead for CheckingReader<'a, R> {
    fn poll_read(
        mut self: core::pin::Pin<&mut Self>,
        cx: &mut core::task::Context<'_>,
        buf: &mut [u8],
    ) -> core::task::Poll<Result<usize, io::Error>> {
        let res = core::pin::Pin::new(&mut self.inner).poll_read(cx, buf);

        if let core::task::Poll::Ready(Ok(len)) = &res {
            self.digest.update(&buf[0..*len]);
        }

        res
    }
}
