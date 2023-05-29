use anyhow::Context as _;
use chacha20poly1305::aead::Aead as _;
use chacha20poly1305::KeyInit as _;
use core::str::FromStr as _;
use num_traits::FromPrimitive as _;
use serde::Deserialize as _;
use std::io::Read as _;
use tokio::io::AsyncReadExt as _;
use tokio::io::AsyncWriteExt as _;

#[derive(
    Clone,
    Debug,
    Eq,
    Hash,
    PartialEq,
    serde::Deserialize,
    serde::Serialize,
    num_derive::FromPrimitive,
)]
#[serde(rename_all = "snake_case")]
enum LeAddrType {
    Public = 0x00,
    Random = 0x01,
    PublicId = 0x02,
    RandomId = 0x03,
    Unresolved = 0xFE,
    Anonymous = 0xFF,
}

fn deserialize_macaddr6<'de, D>(deserializer: D) -> Result<macaddr::MacAddr6, D::Error>
where
    D: serde::de::Deserializer<'de>,
{
    let string = String::deserialize(deserializer)?;

    macaddr::MacAddr6::from_str(&string).map_err(serde::de::Error::custom)
}

fn serialize_macaddr6<S>(data: &macaddr::MacAddr6, serializer: S) -> Result<S::Ok, S::Error>
where
    S: serde::Serializer,
{
    serializer.serialize_str(&format!("{}", data))
}

fn read_key<P: AsRef<std::path::Path>>(path: P) -> anyhow::Result<chacha20poly1305::Key> {
    let mut f = std::fs::File::open(path.as_ref()).context("can't open key file")?;

    let mut data = Vec::with_capacity(32);
    f.read_to_end(&mut data).context("can't read key file")?;

    if data.len() != 32 {
        anyhow::bail!("{} is not a valid key size", data.len());
    }

    Ok(*chacha20poly1305::Key::from_slice(&data))
}

fn deserialize_key<'de, D>(deserializer: D) -> Result<chacha20poly1305::ChaCha20Poly1305, D::Error>
where
    D: serde::de::Deserializer<'de>,
{
    let path = String::deserialize(deserializer)?;
    let key = read_key(path).map_err(serde::de::Error::custom)?;
    Ok(chacha20poly1305::ChaCha20Poly1305::new(&key))
}

#[derive(Clone, Debug, Eq, Hash, PartialEq, serde::Deserialize, serde::Serialize)]
struct LeAddr {
    address_type: LeAddrType,
    #[serde(deserialize_with = "deserialize_macaddr6")]
    #[serde(serialize_with = "serialize_macaddr6")]
    address: macaddr::MacAddr6,
}

impl std::fmt::Display for LeAddr {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{:?}/{}", self.address_type, self.address)
    }
}

#[derive(Debug, serde::Deserialize, num_derive::FromPrimitive)]
#[serde(rename_all = "snake_case")]
enum DeviceType {
    PowerMeter,
}

#[derive(serde::Deserialize)]
struct Device {
    #[serde(flatten)]
    leaddr: LeAddr,
    #[serde(deserialize_with = "deserialize_key")]
    #[serde(rename = "key")]
    cipher: chacha20poly1305::ChaCha20Poly1305,
    #[serde(rename = "type")]
    device_type: DeviceType,
}

fn default_port() -> u16 {
    1883
}

#[derive(serde::Deserialize)]
struct Mqtt {
    host: String,
    #[serde(default = "default_port")]
    port: u16,
}

#[derive(serde::Deserialize)]
struct Config {
    mqtt: Mqtt,

    runtime_dir: Option<std::path::PathBuf>,

    #[serde(rename = "device")]
    devices: std::collections::HashMap<String, Device>,
}

type NonceList = std::collections::HashMap<LeAddr, u128>;
type SharedNonceList = std::sync::Arc<std::sync::Mutex<NonceList>>;

async fn handle_powermeter_frame(
    device: &Device,
    client: &rumqttc::AsyncClient,
    message: &[u8],
) -> anyhow::Result<()> {
    if message.len() != 8 {
        anyhow::bail!("unsupported message len: {}", message.len());
    }

    let active_power = f32::from_le_bytes(message[0..4].try_into().unwrap());
    let active_energy = f32::from_le_bytes(message[4..].try_into().unwrap());

    tracing::info!(
        "active_power={}, active_energy={}",
        active_power,
        active_energy
    );

    if active_power.is_nan() || active_energy.is_nan() || active_energy == 0.0 {
        return Ok(());
    }

    client
        .publish(
            format!("smartmeter/{}/active_power", device.leaddr.address),
            rumqttc::mqttbytes::QoS::AtMostOnce,
            false,
            active_power.to_string(),
        )
        .await
        .context("failed to publish active_power")?;

    client
        .publish(
            format!("smartmeter/{}/active_energy", device.leaddr.address),
            rumqttc::mqttbytes::QoS::AtMostOnce,
            false,
            active_energy.to_string(),
        )
        .await
        .context("failed to publish active_energy")?;

    Ok(())
}

async fn handle_frame(
    config: &Config,
    nonces: &SharedNonceList,
    nonces_tx: &tokio::sync::mpsc::Sender<()>,
    client: &rumqttc::AsyncClient,
    buf: &[u8],
) -> anyhow::Result<()> {
    let address_type = *buf.first().context("missing address type")?;

    let mut address: [u8; 6] = buf
        .get(1..7)
        .context("missing address")?
        .try_into()
        .unwrap();
    address.reverse();

    let leaddr = LeAddr {
        address_type: LeAddrType::from_u8(address_type).context("invalid address type")?,
        address: address.into(),
    };
    let buf = &buf[7..];

    let company_id = buf.get(..2).context("missing company id")?;
    let buf = &buf[2..];

    let nonce = buf.get(..12).context("missing nonce")?;
    let nonce = chacha20poly1305::Nonce::from_slice(nonce);

    let mut nonce_num = [0u8; 16];
    nonce_num.get_mut(..12).unwrap().copy_from_slice(nonce);
    let nonce_num = u128::from_le_bytes(nonce_num);

    let ciphertext = &buf[12..];

    tracing::trace!("address={leaddr} company={company_id:02X?} nonce={nonce:02X?}({nonce_num}) ciphertext={ciphertext:02X?}");

    match nonces.lock().unwrap().entry(leaddr.clone()) {
        std::collections::hash_map::Entry::Occupied(mut entry) => {
            let old_nonce = *entry.get();
            if nonce_num <= old_nonce {
                anyhow::bail!("invalid nonce. old={old_nonce} received={nonce_num}");
            }

            if nonce_num > old_nonce + 1 {
                tracing::debug!("missed {} packets", nonce_num - old_nonce - 1);
            }

            entry.insert(nonce_num);
        }
        std::collections::hash_map::Entry::Vacant(entry) => {
            entry.insert(nonce_num);
        }
    }
    let _ = nonces_tx.try_send(());

    let device = config
        .devices
        .values()
        .find(|device| device.leaddr == leaddr)
        .context("unknown device")?;
    let plaintext = device
        .cipher
        .decrypt(nonce, ciphertext)
        .map_err(|e| anyhow::anyhow!("can't decrypt: {:#}", e))?;

    match device.device_type {
        DeviceType::PowerMeter => handle_powermeter_frame(device, client, &plaintext)
            .await
            .context("can't handle powermeter frame"),
    }
}

fn load_config() -> anyhow::Result<Config> {
    let config_path = std::env::args().nth(1).context("missing config path")?;
    let contents = std::fs::read_to_string(config_path).context("failed to read file")?;
    toml::from_str(&contents).context("failed to parse")
}

fn runtime_dir() -> anyhow::Result<Option<std::path::PathBuf>> {
    match std::env::var("RUNTIME_DIRECTORY") {
        Ok(var) => Ok(Some(var.into())),
        Err(std::env::VarError::NotUnicode(_)) => {
            anyhow::bail!("RUNTIME_DIRECTORY has non-unicode characters");
        }
        Err(std::env::VarError::NotPresent) => Ok(None),
    }
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();

    let config = load_config().context("failed to load config")?;
    let runtime_dir = runtime_dir()?
        .or_else(|| config.runtime_dir.clone())
        .context("can't find any runtime dir")?;
    let nonce_path = runtime_dir.join("nonces");

    let mut mqttoptions =
        rumqttc::MqttOptions::new("smartmeter", &config.mqtt.host, config.mqtt.port);
    mqttoptions.set_keep_alive(std::time::Duration::from_secs(5));

    let (client, mut eventloop) = rumqttc::AsyncClient::new(mqttoptions, 10);

    let socket = tokio::net::UdpSocket::bind("0.0.0.0:8888")
        .await
        .context("can't bind socket")?;

    tokio::task::spawn(async move {
        loop {
            let event = eventloop.poll().await.expect("mqtt error");
            tracing::debug!("mqtt event: {event:?}");
        }
    });

    let bincode_config = bincode::config::standard()
        .with_little_endian()
        .with_variable_int_encoding();

    let nonces = match tokio::fs::File::open(&nonce_path).await {
        Ok(mut file) => {
            let mut data = vec![];
            file.read_to_end(&mut data)
                .await
                .context("failed to read nonce file")?;

            bincode::serde::decode_from_slice(&data, bincode_config)
                .context("failed to deserialize nonces")?
                .0
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => NonceList::new(),
        Err(e) => return Err(e).context("failed to open nonce file"),
    };
    let nonces = std::sync::Arc::new(std::sync::Mutex::new(nonces));

    let nonces2 = nonces.clone();
    let (tx, mut rx) = tokio::sync::mpsc::channel(1);
    tokio::task::spawn(async move {
        let nonce_path = runtime_dir.join("nonces");

        while let Some(()) = rx.recv().await {
            let data = bincode::serde::encode_to_vec(&*nonces2.lock().unwrap(), bincode_config)
                .expect("failed to serialize nonces");

            let mut file = tokio::fs::File::create(&nonce_path)
                .await
                .expect("failed to create nonce file");
            file.write_all(&data)
                .await
                .expect("failed to write nonce file");
        }
    });

    let mut buf = [0; 1024];
    loop {
        let (len, addr) = socket
            .recv_from(&mut buf)
            .await
            .context("can't receive UDP packet")?;
        let buf = &buf[..len];
        tracing::trace!("{len:?} bytes received from {addr:#} buf={buf:02X?}");

        if let Err(e) = handle_frame(&config, &nonces, &tx, &client, buf).await {
            tracing::error!("failed to handle frame: {:#}", e);
        }
    }
}
