//! Receives lora data from UART and forwards it to MQTT for use by homeassistant

use anyhow::anyhow;
use anyhow::Context as _;
use chacha20poly1305::aead::Aead as _;
use chacha20poly1305::aead::NewAead as _;
use clap::Parser as _;
use std::io::Read as _;

#[derive(clap::Parser, Debug)]
#[clap(author, version, about, long_about = None)]
struct Args {
    /// Serial port
    #[clap(short, long, value_parser)]
    port: String,

    /// Baud rate of the serial port
    #[clap(short, long, value_parser)]
    baud_rate: u32,

    /// path to key file
    #[clap(short, long, value_parser)]
    key: String,

    /// URI to mqtt server
    #[clap(short, long, value_parser)]
    mqtt_uri: String,
}

fn handle_message(
    cipher: &chacha20poly1305::ChaCha20Poly1305,
    data: &[u8],
    client: &paho_mqtt::Client,
) {
    log::debug!("message: {:X?}", data);

    if data.len() != 36 {
        log::error!("unsupported message size: {}", data.len());
        return;
    }

    let nonce = chacha20poly1305::Nonce::from_slice(&data[..12]);
    let ciphertext = &data[12..];

    let plaintext = match cipher.decrypt(nonce, ciphertext) {
        Ok(v) => v,
        Err(e) => {
            log::error!("can't decrypt ciphertext: {:#?}", e);
            return;
        }
    };
    log::debug!("plaintext: {:X?}", plaintext);

    if plaintext.len() != 8 {
        log::error!("unsupported plaintext len: {}", plaintext.len());
    }

    let active_power = f32::from_le_bytes(plaintext[0..4].try_into().unwrap());
    let active_energy = f32::from_le_bytes(plaintext[4..].try_into().unwrap());

    log::info!(
        "active_power={}, active_energy={}",
        active_power,
        active_energy
    );

    if active_power.is_nan() || active_energy.is_nan() || active_energy == 0.0 {
        return;
    }

    let msg = paho_mqtt::MessageBuilder::new()
        .topic("smartmeter/power")
        .payload(format!("{}", active_power))
        .qos(1)
        .finalize();
    if let Err(e) = client.publish(msg) {
        println!("can't publish power {:?}", e);
    }

    let msg = paho_mqtt::MessageBuilder::new()
        .topic("smartmeter/energy")
        .payload(format!("{}", active_energy))
        .qos(1)
        .finalize();
    if let Err(e) = client.publish(msg) {
        println!("can't publish energy {:?}", e);
    }
}

fn read_key<P: AsRef<std::path::Path>>(path: P) -> anyhow::Result<chacha20poly1305::Key> {
    let mut f = std::fs::File::open(path.as_ref()).context("can't open key file")?;

    let mut data = Vec::with_capacity(32);
    f.read_to_end(&mut data).context("can't read key file")?;

    if data.len() != 32 {
        return Err(anyhow!("{} is not a valid key size", data.len()));
    }

    Ok(*chacha20poly1305::Key::from_slice(&data))
}

fn main() -> anyhow::Result<()> {
    env_logger::init();
    let args = Args::parse();

    let key = read_key(args.key).context("can't read key")?;
    let cipher = chacha20poly1305::ChaCha20Poly1305::new(&key);

    let mut port = serialport::new(args.port, args.baud_rate)
        .open()
        .expect("failed to open serial port");

    let opts = paho_mqtt::CreateOptionsBuilder::new()
        .server_uri(&args.mqtt_uri)
        .client_id("smartmeter")
        .finalize();
    let mut client = paho_mqtt::Client::new(opts).context("can't create mqtt client")?;
    client.set_timeout(std::time::Duration::from_secs(5));
    client
        .connect(None)
        .context("can't connect to mqtt server")?;

    let mut decoded = vec![0; 100];
    let mut decoder = cobs::CobsDecoder::new(&mut decoded);

    let mut readbuf = vec![0; 100];
    loop {
        let readbuf = match port.read(&mut readbuf) {
            Ok(len) => &readbuf[..len],
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => continue,
            Err(e) => {
                return Err(e).context("failed to read");
            }
        };

        log::debug!("RECEIVED: {:X?}", readbuf);
        for byte in readbuf {
            match decoder.feed(*byte) {
                Ok(None) => continue,
                Ok(Some(len)) => {
                    handle_message(&cipher, &decoded[..len], &client);
                    decoder = cobs::CobsDecoder::new(&mut decoded);
                }
                Err(_len) => {
                    log::error!("decode error, reset");
                    decoder = cobs::CobsDecoder::new(&mut decoded);
                    continue;
                }
            }
        }
    }
}
