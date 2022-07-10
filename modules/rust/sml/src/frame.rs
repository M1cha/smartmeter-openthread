//! handle framing and call [crate::message] module when one was received

use crate::types::FromTlvList as _;
use crate::Error;
use crate::ReaderEnded as _;
use io::AsyncReadExt as _;

struct Buffer {
    inner: [u8; 4],
    read: usize,
}

impl Buffer {
    pub fn new() -> Self {
        Self {
            inner: [0x00; 4],
            read: 0,
        }
    }

    pub fn len(&self) -> usize {
        self.inner.len()
    }

    pub fn read(&self) -> usize {
        self.read
    }

    pub fn set_read(&mut self, read: usize) {
        self.read = read;
    }

    pub fn add_read(&mut self, read: usize) {
        self.read += read;
    }

    pub fn sub_read(&mut self, read: usize) {
        self.read -= read;
    }

    pub fn data_filled(&self) -> &[u8] {
        &self.inner[..self.read]
    }

    pub fn data_free_mut(&mut self) -> &mut [u8] {
        &mut self.inner[self.read..]
    }
}

struct BufferList {
    buffers: [Buffer; 2],
    buffers_swapped: bool,
}

impl BufferList {
    fn get_mut(&mut self, index: usize) -> &mut Buffer {
        let index = index & 0x1;
        let index = if self.buffers_swapped {
            index ^ 0x1
        } else {
            index
        };

        &mut self.buffers[index]
    }

    fn get(&self, index: usize) -> &Buffer {
        let index = index & 0x1;
        let index = if self.buffers_swapped {
            index ^ 0x1
        } else {
            index
        };

        &self.buffers[index]
    }

    fn swap_buffers(&mut self) {
        self.buffers_swapped = !self.buffers_swapped;
    }
}

enum ReaderState {
    ReadBuf,
    ReturnBuf { returned: usize, lastone: bool },
    End,
}

#[pin_project::pin_project]
pub struct CheckingReader<'a, R> {
    #[pin]
    reader: &'a mut R,
    state: ReaderState,
    in_esc: bool,
    bufferlist: BufferList,
    digest: crc::Digest<'static, u16>,
}

impl<'a, R> CheckingReader<'a, R> {
    fn new(reader: &'a mut R) -> Self {
        let mut digest = crate::CRC_INSTANCE.digest();
        digest.update(&[0x1B, 0x1B, 0x1B, 0x1B, 0x01, 0x01, 0x01, 0x01]);

        Self {
            reader,
            state: ReaderState::ReadBuf,
            in_esc: false,
            bufferlist: BufferList {
                buffers: [Buffer::new(), Buffer::new()],
                buffers_swapped: false,
            },
            digest,
        }
    }
}

impl<'a, R> crate::ReaderEnded for CheckingReader<'a, R> {
    fn has_ended(&self) -> bool {
        matches!(self.state, ReaderState::End)
    }
}

impl<'a, R: io::AsyncRead + Unpin> io::AsyncRead for CheckingReader<'a, R> {
    fn poll_read(
        self: core::pin::Pin<&mut Self>,
        cx: &mut core::task::Context<'_>,
        buf: &mut [u8],
    ) -> core::task::Poll<Result<usize, io::Error>> {
        let mut me = self.project();
        loop {
            match &mut me.state {
                ReaderState::ReadBuf => {
                    let in_esc = *me.in_esc;
                    let buffer = me.bufferlist.get_mut(0);

                    let num = futures_util::ready!(me
                        .reader
                        .as_mut()
                        .poll_read(cx, buffer.data_free_mut()))
                    .inspect_err(|_| *me.state = ReaderState::End)?;
                    buffer.add_read(num);

                    if buffer.read() < buffer.len() {
                        continue;
                    }

                    if in_esc {
                        match buffer.data_filled() {
                            &[0x1B, 0x1B, 0x1B, 0x1B] => {
                                *me.in_esc = false;
                                me.digest.update(buffer.data_filled());

                                // return the 4x 0x1B that are already in our buffer
                                me.bufferlist.swap_buffers();
                                if me.bufferlist.get(0).read() > 0 {
                                    *me.state = ReaderState::ReturnBuf {
                                        returned: 0,
                                        lastone: false,
                                    };
                                }
                                continue;
                            }
                            other if other[0] == 0x1A => {
                                let num_fillbytes = other[1];
                                let crc_rec = u16::from_be_bytes(other[2..4].try_into().unwrap())
                                    .swap_bytes();

                                me.digest.update(&other[0..2]);
                                buffer.set_read(0);

                                let mut digest = crate::CRC_INSTANCE.digest();
                                core::mem::swap(&mut digest, me.digest);
                                let crc_calc = digest.finalize();

                                if crc_rec != crc_calc {
                                    log::error!(
                                        "frame CRC mismatch: calc={:X} rec={:X}",
                                        crc_calc,
                                        crc_rec
                                    );
                                    *me.state = ReaderState::End;
                                    return Err(io::Error::Unknown).into();
                                }

                                me.bufferlist.swap_buffers();
                                let buffer = me.bufferlist.get_mut(0);

                                if num_fillbytes > 3 {
                                    log::error!(
                                        "unsupported number of fillbytes: {}",
                                        num_fillbytes
                                    );
                                    *me.state = ReaderState::End;
                                    return Err(io::Error::Unknown).into();
                                }

                                if buffer.read() < num_fillbytes.into() {
                                    log::error!(
                                        "data-length={} can't be less than given fillbytes={}",
                                        buffer.read(),
                                        num_fillbytes
                                    );
                                    *me.state = ReaderState::End;
                                    return Err(io::Error::Unknown).into();
                                }

                                buffer.sub_read(num_fillbytes.into());

                                *me.state = ReaderState::ReturnBuf {
                                    returned: 0,
                                    lastone: true,
                                };
                                continue;
                            }
                            _ => {
                                *me.state = ReaderState::End;
                                return core::task::Poll::Ready(Err(io::Error::Unimplemented));
                            }
                        }
                    }

                    me.digest.update(buffer.data_filled());

                    if buffer.data_filled() == [0x1B, 0x1B, 0x1B, 0x1B] {
                        buffer.set_read(0);
                        *me.in_esc = true;
                        continue;
                    }

                    me.bufferlist.swap_buffers();
                    if me.bufferlist.get(0).read() > 0 {
                        *me.state = ReaderState::ReturnBuf {
                            returned: 0,
                            lastone: false,
                        };
                    }
                }
                ReaderState::ReturnBuf { returned, lastone } => {
                    let buffer = me.bufferlist.get_mut(0);
                    let source = buffer.data_filled();
                    if *returned < source.len() {
                        let copysize = buf.len().min(source.len() - *returned);
                        buf[0..copysize].copy_from_slice(&source[*returned..*returned + copysize]);

                        *returned += copysize;

                        // this allows the frame coder to check if the frame is over
                        if *lastone && *returned == source.len() {
                            buffer.set_read(0);
                            *me.state = ReaderState::End
                        }

                        return core::task::Poll::Ready(Ok(copysize));
                    }

                    buffer.set_read(0);
                    *me.state = if *lastone {
                        ReaderState::End
                    } else {
                        ReaderState::ReadBuf
                    };
                }
                ReaderState::End => {
                    return core::task::Poll::Ready(Ok(0));
                }
            }
        }
    }
}

pub(crate) async fn read_frame<F, R: io::AsyncRead + Unpin>(
    reader: &mut R,
    callback: &mut F,
) -> Result<(), Error>
where
    F: for<'r> crate::Callback<crate::message::CheckingReader<'r, CheckingReader<'r, R>>>,
{
    log::info!("read frame");
    let mut frame = CheckingReader::new(reader);

    let message_crc = crate::message::CheckingReader::new(&mut frame);
    let mut tlv_reader = crate::tlv::Reader::new(message_crc);

    while !tlv_reader.reader().has_ended() {
        log::info!("read message");

        {
            let message = crate::types::Message::from_tlv_list(tlv_reader.read_list().await?);

            let mut field = message.transaction_id().await?;
            let transaction_id = field.parse().await?;
            drop(transaction_id);
            let message = field.finish().await?;

            let (message, group_no) = message.group_no().await?;
            let (message, abort_on_error) = message.abort_on_error().await?;

            log::debug!("group_no={} abort_on_error={}", group_no, abort_on_error);

            let mut field = message.message_body().await?;
            callback.message_received(field.parse().await?).await?;
            let message = field.finish().await?;

            // read in skipped data and calculate the CRC
            message.list.reader.skip_now().await?;
            let crc_calc = message.list.reader.reader().finalize();

            let (message, crc_rec) = message.crc_16().await?;
            message.end_of_sml_msg().await?;

            // For some reason, the crc is not big-endian like TLV numbers generally are.
            // I didn't find anything about that in the documentation.
            if crc_rec.swap_bytes() != crc_calc {
                return Err(Error::ChecksumMismatch {
                    rec: crc_rec,
                    calc: crc_calc,
                });
            }
        }

        tlv_reader.reader().reset();
    }

    Ok(())
}

/// deterministic finite automata waiting for the start marker
pub(crate) async fn wait_for_start_sequence<R: io::AsyncRead + Unpin>(
    reader: &mut R,
) -> Result<(), Error> {
    let mut count = 0;

    while count < 8 {
        let byte = reader.read_u8().await?;
        if (count < 4 && byte == 0x1b) || (count >= 4 && byte == 0x01) {
            count += 1;
        } else if count == 4 && byte == 0x1b {
            // stay in the current state
        } else {
            count = 0;
        }
    }

    Ok(())
}
