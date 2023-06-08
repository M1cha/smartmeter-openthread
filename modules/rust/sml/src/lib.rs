#![cfg_attr(not(feature = "std"), no_std)]
#![feature(type_alias_impl_trait)]
#![feature(assert_matches)]
#![feature(result_option_inspect)]
#![feature(impl_trait_in_assoc_type)]
#![feature(async_fn_in_trait)]
#![feature(impl_trait_projections)]

mod error;
mod frame;
mod macros;
mod message;
mod tlv;

pub use error::Error;

const CRC_16_SML: crc::Algorithm<u16> = crc::Algorithm {
    width: 16,
    poly: 0x1021,
    init: 0xffff,
    refin: true,
    refout: true,
    xorout: 0xFFFF,
    check: 0x4c06,
    residue: 0x0000,
};

const CRC_INSTANCE: crc::Crc<u16> = crc::Crc::<u16>::new(&CRC_16_SML);

#[allow(unused_imports)]
#[allow(unused_variables)]
#[allow(clippy::needless_question_mark)]
#[allow(clippy::enum_variant_names)]
pub mod types {
    pub trait FromTlvList<'a, R> {
        fn from_tlv_list(list: crate::tlv::List<'a, R>) -> Self;
    }

    impl<'a, R> FromTlvList<'a, R> for () {
        fn from_tlv_list(_list: crate::tlv::List<'a, R>) -> Self {}
    }

    pub trait FromTlvItem<'a, R>: Sized {
        async fn from_tlv_item(list: crate::tlv::Item<'a, R>) -> Result<Self, crate::Error>;
    }

    macro_rules! impl_fromtlvitem {
        ($name:ident, $tlvty:ident, $intofn:ident) => {
            impl<'a, R: io::AsyncRead + Unpin + 'a> FromTlvItem<'a, R> for $name {
                async fn from_tlv_item(
                    item: crate::tlv::Item<'a, R>,
                ) -> Result<Self, crate::Error> {
                    match item {
                        crate::tlv::Item::$tlvty(v) => Ok(v.$intofn().await?),
                        _ => Err(crate::Error::UnexpectedValue),
                    }
                }
            }
        };
    }

    impl_fromtlvitem!(u8, Unsigned, into_u8);
    impl_fromtlvitem!(u16, Unsigned, into_u16);
    impl_fromtlvitem!(u32, Unsigned, into_u32_relaxed);
    impl_fromtlvitem!(u64, Unsigned, into_u64_relaxed);

    impl_fromtlvitem!(i8, Integer, into_i8);
    impl_fromtlvitem!(i16, Integer, into_i16);
    impl_fromtlvitem!(i32, Integer, into_i32_relaxed);
    impl_fromtlvitem!(i64, Integer, into_i64_relaxed);

    impl_fromtlvitem!(bool, Boolean, into_bool);

    pub trait ParseField<'r, 'b, R>
    where
        Self: Sized,
    {
        async fn parse_field<'s: 'b>(
            list: &'s mut crate::tlv::List<'r, R>,
        ) -> Result<Self, crate::Error>;
    }

    impl<'r: 'b, 'b, R: io::AsyncRead + Unpin + 'r> ParseField<'r, 'b, R>
        for crate::tlv::String<'b, R>
    {
        async fn parse_field<'l: 'b>(
            list: &'l mut crate::tlv::List<'r, R>,
        ) -> Result<Self, crate::Error> {
            list.next_string().await
        }
    }

    impl<'a, R: io::AsyncRead + Unpin + 'a> FromTlvItem<'a, R> for crate::tlv::String<'a, R> {
        async fn from_tlv_item(item: crate::tlv::Item<'a, R>) -> Result<Self, crate::Error> {
            match item {
                crate::tlv::Item::String(s) => Ok(s),
                _ => Err(crate::Error::UnexpectedValue),
            }
        }
    }

    include!(concat!(env!("OUT_DIR"), "/messages.rs"));

    impl<'a, R> ValueEnum<'a, R> {
        pub fn into_i128_relaxed(self) -> Result<i128, crate::Error> {
            Ok(match self {
                Self::N8BitInteger(n) => n.into(),
                Self::N16BitInteger(n) => n.into(),
                Self::N32BitInteger(n) => n.into(),
                Self::N64BitInteger(n) => n.into(),
                Self::N8BitUnsigned(n) => n.into(),
                Self::N16BitUnsigned(n) => n.into(),
                Self::N32BitUnsigned(n) => n.into(),
                Self::N64BitUnsigned(n) => n.into(),
                _ => return Err(crate::Error::UnexpectedValue),
            })
        }

        pub fn into_u64_relaxed(self) -> Result<u64, crate::Error> {
            Ok(match self {
                Self::N8BitUnsigned(n) => n.into(),
                Self::N16BitUnsigned(n) => n.into(),
                Self::N32BitUnsigned(n) => n.into(),
                Self::N64BitUnsigned(n) => n,
                _ => return Err(crate::Error::UnexpectedValue),
            })
        }

        pub fn into_64_relaxed(self) -> Result<i64, crate::Error> {
            Ok(match self {
                Self::N8BitInteger(n) => n.into(),
                Self::N16BitInteger(n) => n.into(),
                Self::N32BitInteger(n) => n.into(),
                Self::N64BitInteger(n) => n,
                _ => return Err(crate::Error::UnexpectedValue),
            })
        }
    }
}

trait ReaderEnded {
    fn has_ended(&self) -> bool;
}

/// callbacks for received SML messages
pub trait Callback<R> {
    fn frame_start(&mut self);
    async fn message_received<'a>(
        &'a mut self,
        val: types::MessageBody<'a, R>,
    ) -> Result<(), Error>;
    fn frame_finished(&mut self, valid: bool);
}

pub async fn task<R, C>(reader: &mut R, callback: &mut C) -> Result<(), Error>
where
    R: io::AsyncRead + Unpin,
    C: for<'r> Callback<message::CheckingReader<'r, frame::CheckingReader<'r, R>>>,
{
    loop {
        crate::frame::wait_for_start_sequence(reader).await?;

        callback.frame_start();

        match crate::frame::read_frame(reader, callback).await {
            Ok(()) => callback.frame_finished(true),
            Err(e) => {
                log::error!("failed to read frame: {:#?}", e);
                callback.frame_finished(false);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    struct TestCallback;

    const OBIS_ACTIVE_ENERGY: &[u8] = &[0x01, 0x00, 0x01, 0x08, 0x00, 0xFF];
    const OBIS_ACTIVE_POWER: &[u8] = &[0x01, 0x00, 0x10, 0x07, 0x00, 0xFF];

    impl<R: io::AsyncRead + Unpin> crate::Callback<R> for TestCallback {
        async fn message_received<'a>(
            &'a mut self,
            mut body: crate::types::MessageBody<'a, R>,
        ) -> Result<(), crate::Error> {
            log::info!("got message body");

            match body.read().await? {
                crate::types::MessageBodyEnum::GetListResponse(r) => {
                    log::info!("got list response");

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
                                log::debug!("active_power={}", value.into_i128_relaxed()?);
                            }

                            OBIS_ACTIVE_ENERGY => {
                                log::debug!("active_energy={}", value.into_i128_relaxed()?);
                            }
                            _ => continue,
                        }
                    }
                }
                _ => return Ok(()),
            }

            Ok(())
        }

        fn frame_start(&mut self) {}

        fn frame_finished(&mut self, _valid: bool) {}
    }

    #[test_log::test(tokio::test)]
    async fn basic() {
        let mut callback = TestCallback {};

        let sampledata = include_bytes!("/home/m1cha/nbu/west_smartmeter_lora/sampledata.bin");
        let mut reader = io::FuturesUtilReader(futures_util::io::Cursor::new(&sampledata[..]));

        loop {
            crate::frame::wait_for_start_sequence(&mut reader)
                .await
                .unwrap();
            crate::frame::read_frame(&mut reader, &mut callback)
                .await
                .unwrap();

            break;
        }
    }
}
