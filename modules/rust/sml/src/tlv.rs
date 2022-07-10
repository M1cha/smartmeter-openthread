//! implements SMLs TLV protocol

use crate::Error;
use io::AsyncReadExt as _;

crate::macros::bitvalues! {
    #[derive(Debug)]
    pub enum TlvType : u8 {
        String = 0b000,
        Boolean = 0b100,
        Integer = 0b101,
        Unsigned = 0b110,
        List = 0b111,
    }
}

bitfield::bitfield! {
    struct TlvHeader(u8);
    impl Debug;
    has_more, _: 7;
    ty_raw, _: 6, 4;
    len, _: 3, 0;
}

impl TlvHeader {
    pub fn ty(&self) -> TlvType {
        self.ty_raw().into()
    }
}

pub struct String<'a, R> {
    reader: &'a mut Reader<R>,
    pub(crate) len: usize,
}

impl<'a, R: io::AsyncRead + Unpin> String<'a, R> {
    pub fn len(&self) -> usize {
        self.len
    }

    /// read the whole string to `buf`
    ///
    /// `buf` must be exactly the size of the string.
    /// You can get the length through the `len` function.
    pub async fn read(&mut self, buf: &mut [u8]) -> Result<(), Error> {
        if buf.len() != self.len() {
            return Err(Error::WrongBufferSize);
        }

        self.reader.reader().read_exact(buf).await?;
        self.len = 0;

        Ok(())
    }
}

impl<'a, R> Drop for String<'a, R> {
    fn drop(&mut self) {
        log::trace!("drop string of length {}", self.len);
        self.reader.remaining_bytes += self.len;
    }
}

pub struct Boolean<'a, R> {
    reader: &'a mut Reader<R>,
    pub(crate) len: usize,
}

impl<'a, R> Drop for Boolean<'a, R> {
    fn drop(&mut self) {
        self.reader.remaining_bytes += self.len;
    }
}

impl<'a, R: io::AsyncRead + Unpin> Boolean<'a, R> {
    pub async fn into_bool(mut self) -> Result<bool, Error> {
        if self.len != 1 {
            Err(Error::UnsupportedLen { len: self.len })
        } else {
            let v = self.reader.reader().read_u8().await?;
            self.len = 0;
            Ok(v != 0x00)
        }
    }
}

macro_rules! impl_into_ty {
    ($name:ident, $minsz:literal, $ty:ty, $signed:literal) => {
        pub async fn $name(mut self) -> Result<$ty, Error> {
            const MAXSZ: usize = core::mem::size_of::<$ty>();
            match self.len {
                $minsz..=MAXSZ => {
                    let mut buf = [0x00; MAXSZ];
                    let (fillbuf, mut readbuf) = buf.split_at_mut(MAXSZ - self.len);
                    self.reader.reader().read_exact(&mut readbuf).await?;
                    self.len = 0;

                    // sign-extend
                    if $signed && (readbuf[0] & 0b10000000) != 0 {
                        fillbuf.fill(0xff);
                    }

                    Ok(<$ty>::from_be_bytes(buf))
                }
                _ => Err(Error::UnsupportedLen { len: self.len }),
            }
        }
    };
}

pub struct Integer<'a, R> {
    reader: &'a mut Reader<R>,
    pub(crate) len: usize,
}

impl<'a, R> Drop for Integer<'a, R> {
    fn drop(&mut self) {
        self.reader.remaining_bytes += self.len;
    }
}

impl<'a, R: io::AsyncRead + Unpin> Integer<'a, R> {
    impl_into_ty!(into_i8, 1, i8, true);
    impl_into_ty!(into_i16, 2, i16, true);
    impl_into_ty!(into_i32, 3, i32, true);
    impl_into_ty!(into_i64, 5, i64, true);

    pub async fn into_i32_relaxed(self) -> Result<i32, Error> {
        match self.len {
            1 => self.into_i8().await.map(|v| v.into()),
            2 => self.into_i16().await.map(|v| v.into()),
            3..=4 => self.into_i32().await,
            other => Err(Error::UnsupportedLen { len: other }),
        }
    }

    pub async fn into_i64_relaxed(self) -> Result<i64, Error> {
        match self.len {
            1 => self.into_i8().await.map(|v| v.into()),
            2 => self.into_i16().await.map(|v| v.into()),
            3..=4 => self.into_i32().await.map(|v| v.into()),
            5..=8 => self.into_i64().await,
            other => Err(Error::UnsupportedLen { len: other }),
        }
    }
}

pub struct Unsigned<'a, R> {
    reader: &'a mut Reader<R>,
    pub(crate) len: usize,
}

impl<'a, R> Drop for Unsigned<'a, R> {
    fn drop(&mut self) {
        self.reader.remaining_bytes += self.len;
    }
}

impl<'a, R: io::AsyncRead + Unpin> Unsigned<'a, R> {
    impl_into_ty!(into_u8, 1, u8, false);
    impl_into_ty!(into_u16, 2, u16, false);
    impl_into_ty!(into_u32, 3, u32, false);
    impl_into_ty!(into_u64, 5, u64, false);

    pub async fn into_u32_relaxed(self) -> Result<u32, Error> {
        match self.len {
            1 => self.into_u8().await.map(|v| v.into()),
            2 => self.into_u16().await.map(|v| v.into()),
            3..=4 => self.into_u32().await,
            other => Err(Error::UnsupportedLen { len: other }),
        }
    }

    pub async fn into_u64_relaxed(self) -> Result<u64, Error> {
        match self.len {
            1 => self.into_u8().await.map(|v| v.into()),
            2 => self.into_u16().await.map(|v| v.into()),
            3..=4 => self.into_u32().await.map(|v| v.into()),
            5..=8 => self.into_u64().await,
            other => Err(Error::UnsupportedLen { len: other }),
        }
    }
}

pub struct List<'a, R> {
    pub(crate) reader: &'a mut Reader<R>,
    pub(crate) len: usize,
}
impl<'a, R> Drop for List<'a, R> {
    fn drop(&mut self) {
        log::trace!("drop list of length {}", self.len);
        self.reader.remaining_tlvs += self.len;
    }
}

macro_rules! impl_next_variant {
    ($name:ident, $variant:ident) => {
        #[doc = "decode specified variant and return an error if something else was received"]
        #[allow(dead_code)]
        pub async fn $name<'b>(&'b mut self) -> Result<$variant<'b, R>, Error> {
            match self.next_any().await? {
                Item::$variant(v) => Ok(v),
                Item::None => Err(Error::NoneTlv),
                _ => Err(Error::UnexpectedValue),
            }
        }
    };
}

macro_rules! impl_next_variant_opt {
    ($name:ident, $variant:ident) => {
        #[allow(dead_code)]
        #[doc = "decode specified variant and return an error if something else was received\n\nReturns [None] when a 0-length string was received"]
        pub async fn $name<'b>(&'b mut self) -> Result<Option<$variant<'b, R>>, Error> {
            match self.next_any().await? {
                Item::$variant(v) => Ok(Some(v)),
                Item::None => Ok(None),
                _ => Err(Error::UnexpectedValue),
            }
        }
    };
}

impl<'a, R: io::AsyncRead + Unpin> List<'a, R> {
    pub fn len(&self) -> usize {
        self.len
    }

    pub async fn next(&mut self) -> Result<Option<Item<'_, R>>, Error> {
        if self.len == 0 {
            Ok(None)
        } else {
            let (ty, len) = self.reader.read_tlv().await?;
            self.len -= 1;

            Ok(Some(match ty {
                TlvType::String if len == 0 => Item::None,
                TlvType::String => Item::String(String {
                    reader: &mut self.reader,
                    len,
                }),
                TlvType::Boolean => Item::Boolean(Boolean {
                    reader: &mut self.reader,
                    len,
                }),
                TlvType::Integer => Item::Integer(Integer {
                    reader: &mut self.reader,
                    len,
                }),
                TlvType::Unsigned => Item::Unsigned(Unsigned {
                    reader: &mut self.reader,
                    len,
                }),
                TlvType::List => Item::List(List {
                    reader: &mut self.reader,
                    len,
                }),
                TlvType::Other(ty) => return Err(Error::UnsupportedTlvType { ty }),
            }))
        }
    }

    pub async fn next_any<'b>(&'b mut self) -> Result<Item<'b, R>, Error> {
        match self.next().await? {
            Some(v) => Ok(v),
            None => Err(Error::EndOfList),
        }
    }

    pub async fn next_end(&mut self) -> Result<(), Error> {
        match self.next().await {
            Err(Error::EndOfSmlMessage) => (),
            Err(e) => return Err(e),
            Ok(_) => return Err(Error::UnexpectedValue),
        }

        self.len -= 1;
        Ok(())
    }

    pub async fn skip(&mut self, num: usize) -> Result<(), Error> {
        if num > self.len {
            return Err(Error::EndOfList);
        }

        self.reader.remaining_tlvs += num;
        self.len -= num;

        Ok(())
    }

    impl_next_variant!(next_string, String);
    impl_next_variant!(next_boolean, Boolean);
    impl_next_variant!(next_integer, Integer);
    impl_next_variant!(next_unsigned, Unsigned);
    impl_next_variant!(next_list, List);

    impl_next_variant_opt!(next_string_opt, String);
    impl_next_variant_opt!(next_boolean_opt, Boolean);
    impl_next_variant_opt!(next_integer_opt, Integer);
    impl_next_variant_opt!(next_unsigned_opt, Unsigned);
    impl_next_variant_opt!(next_list_opt, List);
}

pub enum Item<'a, R> {
    String(String<'a, R>),
    Boolean(Boolean<'a, R>),
    Integer(Integer<'a, R>),
    Unsigned(Unsigned<'a, R>),
    List(List<'a, R>),
    None,
}

pub struct Reader<R> {
    reader: R,
    remaining_bytes: usize,
    remaining_tlvs: usize,
}

impl<R: io::AsyncRead + Unpin> Reader<R> {
    pub fn new(reader: R) -> Self {
        Self {
            reader,
            remaining_bytes: 0,
            remaining_tlvs: 0,
        }
    }

    pub fn reader(&mut self) -> &mut R {
        &mut self.reader
    }

    async fn read_tlv_inner(&mut self) -> Result<(TlvType, usize), Error> {
        let mut header = TlvHeader(self.reader.read_u8().await?);

        let ty = header.ty();
        let mut len: usize = header.len().into();
        let mut header_len = 1;

        while header.has_more() {
            header = TlvHeader(self.reader.read_u8().await?);
            if header.ty_raw() != 0 {
                return Err(Error::MultibyteTlvReservedType {
                    ty: header.ty_raw(),
                });
            }

            len = len.checked_shl(4).ok_or(Error::TlvLengthTooBig)?;
            len = len
                .checked_add(header.len().into())
                .ok_or(Error::TlvLengthTooBig)?;

            header_len += 1;
        }

        match header.ty() {
            // for lists the length doesn't include the header
            TlvType::List => (),
            TlvType::String if len == 0 => return Err(Error::EndOfSmlMessage),
            _ => {
                if len == 0 {
                    return Err(Error::ShortTlvLength { len });
                } else {
                    len = len
                        .checked_sub(header_len)
                        .ok_or(Error::ShortTlvLength { len })?;
                }
            }
        }

        log::trace!("({:?}, {})", ty, len);
        Ok((ty, len))
    }

    pub async fn skip_now(&mut self) -> Result<(), Error> {
        if self.remaining_bytes > 0 {
            self.skip_bytes(self.remaining_bytes).await?;
            self.remaining_bytes = 0;
        }

        if self.remaining_tlvs > 0 {
            self.skip_tlvs(self.remaining_tlvs).await?;
            self.remaining_tlvs = 0;
        }

        Ok(())
    }

    async fn read_tlv(&mut self) -> Result<(TlvType, usize), Error> {
        //log::trace!("read tlv");
        self.skip_now().await?;
        self.read_tlv_inner().await
    }

    pub async fn read_list(&mut self) -> Result<List<'_, R>, Error> {
        let (ty, len) = self.read_tlv().await?;
        match ty {
            TlvType::List => Ok(List { reader: self, len }),
            _ => Err(Error::UnexpectedTlv { ty, len }),
        }
    }

    async fn skip_bytes(&mut self, mut num: usize) -> Result<(), Error> {
        log::trace!("skip {} bytes", num);

        let mut buf = [0u8; 4];

        while num > 0 {
            let readlen = num.min(4);

            self.reader.read_exact(&mut buf[0..readlen]).await?;

            num -= readlen;
        }

        Ok(())
    }

    /// skip the given number of TLVs recursively
    ///
    /// for list TLVs, all the list items are skipped as well
    async fn skip_tlvs(&mut self, mut num: usize) -> Result<(), Error> {
        log::trace!("skip {} tlvs", num);

        while num > 0 {
            num -= 1;

            let (ty, len) = match self.read_tlv_inner().await {
                Err(Error::EndOfSmlMessage) => {
                    return if num == 0 {
                        Err(Error::EndOfSmlMessage)
                    } else {
                        Err(Error::MidMessageEndMarker { remaining: num })
                    };
                }
                Err(e) => return Err(e),
                Ok(v) => v,
            };

            match &ty {
                TlvType::List => {
                    // For skipping the length doesn't matter.
                    // Pretend we're processing a longer list
                    num += len;
                }
                _ => {
                    self.skip_bytes(len).await?;
                }
            }
        }

        log::trace!("done");
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use core::assert_matches::debug_assert_matches;

    fn make_reader(buf: &[u8]) -> super::Reader<impl io::AsyncRead + Unpin + '_> {
        super::Reader::new(io::FuturesUtilReader(futures_util::io::Cursor::new(buf)))
    }

    macro_rules! make_item_reader {
        ($name:path, $value:expr) => {{
            $name {
                reader: &mut { make_reader($value) },
                len: $value.len(),
            }
        }};
    }

    static UNSIGNED_TESTS: &[(&[u8], Option<u8>, Option<u16>, Option<u32>, Option<u64>)] = &[
        (&[], None, None, None, None),
        (&[0xaa], Some(0xaa), None, None, None),
        (&[0xaa, 0xbb], None, Some(0xaabb), None, None),
        (&[0xaa, 0xbb, 0xcc], None, None, Some(0xaabbcc), None),
        (
            &[0xaa, 0xbb, 0xcc, 0xdd],
            None,
            None,
            Some(0xaabbccdd),
            None,
        ),
        (
            &[0xaa, 0xbb, 0xcc, 0xdd, 0xee],
            None,
            None,
            None,
            Some(0xaabbccddee),
        ),
        (
            &[0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff],
            None,
            None,
            None,
            Some(0xaabbccddeeff),
        ),
        (
            &[0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11],
            None,
            None,
            None,
            Some(0xaabbccddeeff11),
        ),
        (
            &[0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22],
            None,
            None,
            None,
            Some(0xaabbccddeeff1122),
        ),
        (
            &[0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22, 0x33],
            None,
            None,
            None,
            None,
        ),
    ];

    #[test_log::test(tokio::test)]
    async fn unsigned() {
        for (buf, u8val, u16val, u32val, u64val) in UNSIGNED_TESTS {
            println!("test with buf: {:X?}", buf);

            let res = make_item_reader!(super::Unsigned, buf).into_u8().await;
            match u8val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }

            let res = make_item_reader!(super::Unsigned, buf).into_u16().await;
            match u16val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }

            let res = make_item_reader!(super::Unsigned, buf).into_u32().await;
            match u32val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }

            let res = make_item_reader!(super::Unsigned, buf).into_u64().await;
            match u64val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }
        }
    }

    static SIGNED_TESTS: &[(&[u8], Option<i8>, Option<i16>, Option<i32>, Option<i64>)] = &[
        (&[], None, None, None, None),
        // positive numbers which fit into a signed int
        (&[0x7f], Some(0x7f), None, None, None),
        (&[0x7f, 0xbb], None, Some(0x7fbb), None, None),
        (&[0x7f, 0xbb, 0xcc], None, None, Some(0x7fbbcc), None),
        (
            &[0x7f, 0xbb, 0xcc, 0xdd],
            None,
            None,
            Some(0x7fbbccdd),
            None,
        ),
        (
            &[0x7f, 0xbb, 0xcc, 0xdd, 0xee],
            None,
            None,
            None,
            Some(0x7fbbccddee),
        ),
        (
            &[0x7f, 0xbb, 0xcc, 0xdd, 0xee, 0xff],
            None,
            None,
            None,
            Some(0x7fbbccddeeff),
        ),
        (
            &[0x7f, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11],
            None,
            None,
            None,
            Some(0x7fbbccddeeff11),
        ),
        (
            &[0x7f, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22],
            None,
            None,
            None,
            Some(0x7fbbccddeeff1122),
        ),
        (
            &[0x7f, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x11, 0x22, 0x33],
            None,
            None,
            None,
            None,
        ),
        (&[0xd6], Some(-42), None, None, None),
        (&[0xff, 0xd6], None, Some(-42), None, None),
        (&[0xff, 0xff, 0xd6], None, None, Some(-42), None),
        (&[0xff, 0xff, 0xff, 0xd6], None, None, Some(-42), None),
        (&[0xff, 0xff, 0xff, 0xff, 0xd6], None, None, None, Some(-42)),
        (
            &[0xff, 0xff, 0xff, 0xff, 0xff, 0xd6],
            None,
            None,
            None,
            Some(-42),
        ),
        (
            &[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd6],
            None,
            None,
            None,
            Some(-42),
        ),
        (
            &[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd6],
            None,
            None,
            None,
            Some(-42),
        ),
        (
            &[0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd6],
            None,
            None,
            None,
            None,
        ),
    ];

    #[test_log::test(tokio::test)]
    async fn signed() {
        for (buf, i8val, i16val, i32val, i64val) in SIGNED_TESTS {
            println!("test with buf: {:X?}", buf);

            let res = make_item_reader!(super::Integer, buf).into_i8().await;
            match i8val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }

            let res = make_item_reader!(super::Integer, buf).into_i16().await;
            match i16val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }

            let res = make_item_reader!(super::Integer, buf).into_i32().await;
            match i32val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }

            let res = make_item_reader!(super::Integer, buf).into_i64().await;
            match i64val {
                Some(v1) => debug_assert_matches!(res, Ok(v2) if v1 == &v2),
                None => {
                    debug_assert_matches!(res, Err(crate::Error::UnsupportedLen { len }) if len ==  buf.len())
                }
            }
        }
    }
}
