//! App-level pairing over the BLE Control characteristic (docs/ble-protocol.md).
//!
//! Numeric comparison with a commitment, so a man-in-the-middle cannot grind
//! nonces until the six-digit codes match:
//!
//! 1. host -> device  plain `{t:"hello", pk, c}`  c = SHA-256("cprec v1 commit" | pk_h | n_h)
//! 2. device -> host  plain `{t:"hello", pk, n, known}`
//! 3. host -> device  plain `{t:"reveal", n}`    device checks c
//! 4. both: ECDH + HKDF session key; code from SHA-256 of the transcript.
//!
//! Everything after step 3 is AES-256-GCM with per-direction counters.
//! The transport (BLE, or channels in tests) moves whole message frames;
//! fragmentation is in [`fragment`] / [`Reassembler`].

use std::{future::Future, time::Duration};

use aes_gcm::{
    Aes256Gcm, KeyInit,
    aead::{Aead, Payload},
};
use ciborium::Value;
use hkdf::Hkdf;
use p256::{PublicKey, SecretKey, ecdh::diffie_hellman, elliptic_curve::sec1::ToEncodedPoint};
use rand_core::{OsRng, RngCore};
use sha2::{Digest, Sha256};

pub const SERVICE_UUID: &str = "6f1c0001-5b2e-4c8f-9a4d-3e7b1f2a9c00";
pub const INFO_UUID: &str = "6f1c0002-5b2e-4c8f-9a4d-3e7b1f2a9c00";
pub const CONTROL_UUID: &str = "6f1c0003-5b2e-4c8f-9a4d-3e7b1f2a9c00";
/// Manufacturer data in the scan response: company 0xFFFF, "CP", flags,
/// 8-byte device id. Flags bit 0: pairing mode; bit 1: has a peer.
pub const MFG_COMPANY: u16 = 0xFFFF;
pub const MFG_FLAG_PAIRING: u8 = 0x01;
pub const MFG_FLAG_PAIRED: u8 = 0x02;

const FRAME_PLAIN: u8 = 0x00;
const FRAME_SECURE: u8 = 0x01;
const DIR_HOST: u8 = b'H';
const DIR_DEVICE: u8 = b'D';
/// Largest reassembled message either side accepts.
pub const MAX_MESSAGE: usize = 2048;

#[derive(Debug, Clone, thiserror::Error, PartialEq)]
pub enum PairError {
    #[error("Bluetooth: {0}")]
    Transport(String),
    #[error("the device did not answer in time")]
    Timeout,
    #[error("the device said: {0}")]
    Device(String),
    #[error("unexpected message: {0}")]
    Protocol(String),
    #[error("message failed authentication")]
    Crypto,
}

pub type PairResult<T> = Result<T, PairError>;

// --- primitives -------------------------------------------------------------

pub type PublicKeyBytes = [u8; 65];

pub fn commitment(host_pk: &PublicKeyBytes, host_nonce: &[u8; 16]) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(b"cprec v1 commit");
    h.update(host_pk);
    h.update(host_nonce);
    h.finalize().into()
}

pub fn session_key(
    shared: &[u8; 32],
    host_pk: &PublicKeyBytes,
    device_pk: &PublicKeyBytes,
    host_nonce: &[u8; 16],
    device_nonce: &[u8; 16],
) -> [u8; 32] {
    let mut salt = [0u8; 32];
    salt[..16].copy_from_slice(host_nonce);
    salt[16..].copy_from_slice(device_nonce);
    let mut info = Vec::with_capacity(16 + 130);
    info.extend_from_slice(b"cprec v1 session");
    info.extend_from_slice(host_pk);
    info.extend_from_slice(device_pk);
    let mut key = [0u8; 32];
    Hkdf::<Sha256>::new(Some(&salt), shared).expand(&info, &mut key).expect("32 bytes is a valid length");
    key
}

/// Six digits both screens show. Built only from public values, which is
/// what numeric comparison needs: a man-in-the-middle changes the keys each
/// side sees, so the codes differ.
pub fn comparison_code(
    host_pk: &PublicKeyBytes,
    device_pk: &PublicKeyBytes,
    host_nonce: &[u8; 16],
    device_nonce: &[u8; 16],
) -> String {
    let mut h = Sha256::new();
    h.update(b"cprec v1 code");
    h.update(host_pk);
    h.update(device_pk);
    h.update(host_nonce);
    h.update(device_nonce);
    let d = h.finalize();
    let n = u32::from_be_bytes([d[0], d[1], d[2], d[3]]) % 1_000_000;
    format!("{n:06}")
}

/// First 16 hex chars of SHA-256 of an uncompressed P-256 key. Same rule
/// for device ids and host ids.
pub fn key_id(pk: &PublicKeyBytes) -> String {
    Sha256::digest(pk)[..8].iter().map(|b| format!("{b:02x}")).collect()
}

fn gcm_nonce(direction: u8, counter: u64) -> [u8; 12] {
    let mut n = [0u8; 12];
    n[0] = direction;
    n[4..].copy_from_slice(&counter.to_be_bytes());
    n
}

pub fn public_bytes(secret: &SecretKey) -> PublicKeyBytes {
    let point = secret.public_key().to_encoded_point(false);
    point.as_bytes().try_into().expect("uncompressed P-256 point is 65 bytes")
}

pub fn ecdh(secret: &SecretKey, peer: &PublicKeyBytes) -> PairResult<[u8; 32]> {
    let peer = PublicKey::from_sec1_bytes(peer).map_err(|_| PairError::Protocol("bad public key".into()))?;
    let shared = diffie_hellman(secret.to_nonzero_scalar(), peer.as_affine());
    Ok((*shared.raw_secret_bytes()).into())
}

fn random<const N: usize>() -> [u8; N] {
    let mut b = [0u8; N];
    OsRng.fill_bytes(&mut b);
    b
}

// --- framing ----------------------------------------------------------------

/// Splits a message into BLE writes/notifications. Byte 0 of each fragment:
/// 1 = last fragment, 0 = more follow.
pub fn fragment(message: &[u8], max_fragment: usize) -> Vec<Vec<u8>> {
    let body = max_fragment.max(2) - 1;
    let chunks: Vec<&[u8]> = if message.is_empty() { vec![&[][..]] } else { message.chunks(body).collect() };
    let last = chunks.len() - 1;
    chunks
        .into_iter()
        .enumerate()
        .map(|(i, c)| {
            let mut f = Vec::with_capacity(c.len() + 1);
            f.push(u8::from(i == last));
            f.extend_from_slice(c);
            f
        })
        .collect()
}

#[derive(Default)]
pub struct Reassembler {
    buf: Vec<u8>,
}

impl Reassembler {
    pub fn push(&mut self, fragment: &[u8]) -> PairResult<Option<Vec<u8>>> {
        let (&flag, body) = fragment.split_first().ok_or_else(|| PairError::Protocol("empty fragment".into()))?;
        if self.buf.len() + body.len() > MAX_MESSAGE {
            self.buf.clear();
            return Err(PairError::Protocol("message too long".into()));
        }
        self.buf.extend_from_slice(body);
        Ok(if flag & 1 == 1 { Some(std::mem::take(&mut self.buf)) } else { None })
    }
}

// --- CBOR messages ----------------------------------------------------------

pub fn encode(entries: Vec<(&str, Value)>) -> Vec<u8> {
    let map = Value::Map(entries.into_iter().map(|(k, v)| (Value::Text(k.into()), v)).collect());
    let mut out = Vec::new();
    ciborium::into_writer(&map, &mut out).expect("writing to a Vec cannot fail");
    out
}

#[derive(Debug, Clone)]
pub struct Message {
    pub kind: String,
    fields: Vec<(Value, Value)>,
}

impl Message {
    pub fn parse(bytes: &[u8]) -> PairResult<Self> {
        let value: Value = ciborium::from_reader(bytes).map_err(|e| PairError::Protocol(format!("CBOR: {e}")))?;
        let Value::Map(fields) = value else {
            return Err(PairError::Protocol("not a map".into()));
        };
        let mut msg = Message { kind: String::new(), fields };
        msg.kind = msg.text("t").ok_or_else(|| PairError::Protocol("no type".into()))?;
        Ok(msg)
    }

    fn get(&self, key: &str) -> Option<&Value> {
        self.fields.iter().find(|(k, _)| k.as_text() == Some(key)).map(|(_, v)| v)
    }
    pub fn text(&self, key: &str) -> Option<String> {
        self.get(key)?.as_text().map(str::to_owned)
    }
    pub fn bytes(&self, key: &str) -> Option<Vec<u8>> {
        self.get(key)?.as_bytes().cloned()
    }
    pub fn fixed<const N: usize>(&self, key: &str) -> PairResult<[u8; N]> {
        self.bytes(key)
            .and_then(|b| b.try_into().ok())
            .ok_or_else(|| PairError::Protocol(format!("missing or bad '{key}'")))
    }
    pub fn flag(&self, key: &str) -> bool {
        self.get(key).and_then(Value::as_bool).unwrap_or(false)
    }
    pub fn texts(&self, key: &str) -> Vec<String> {
        match self.get(key) {
            Some(Value::Array(items)) => items.iter().filter_map(|v| v.as_text().map(str::to_owned)).collect(),
            _ => vec![],
        }
    }
}

pub fn plain_frame(cbor: &[u8]) -> Vec<u8> {
    let mut f = Vec::with_capacity(cbor.len() + 1);
    f.push(FRAME_PLAIN);
    f.extend_from_slice(cbor);
    f
}

/// AES-256-GCM with a counter per direction. Frame: 0x01 | counter (8, BE) |
/// ciphertext+tag; the 9-byte header is the AAD. Counters must increase, so
/// a replayed frame is rejected.
pub struct SecureChannel {
    cipher: Aes256Gcm,
    send_dir: u8,
    recv_dir: u8,
    send_counter: u64,
    recv_counter: u64,
}

impl SecureChannel {
    pub fn new(key: &[u8; 32], is_host: bool) -> Self {
        let (send_dir, recv_dir) = if is_host { (DIR_HOST, DIR_DEVICE) } else { (DIR_DEVICE, DIR_HOST) };
        Self {
            cipher: Aes256Gcm::new(key.into()),
            send_dir,
            recv_dir,
            send_counter: 0,
            recv_counter: 0,
        }
    }

    pub fn seal(&mut self, cbor: &[u8]) -> Vec<u8> {
        self.send_counter += 1;
        let mut frame = vec![FRAME_SECURE];
        frame.extend_from_slice(&self.send_counter.to_be_bytes());
        let nonce = gcm_nonce(self.send_dir, self.send_counter);
        let ct = self
            .cipher
            .encrypt((&nonce).into(), Payload { msg: cbor, aad: &frame })
            .expect("AES-GCM encryption of a small message cannot fail");
        frame.extend_from_slice(&ct);
        frame
    }

    pub fn open(&mut self, frame: &[u8]) -> PairResult<Vec<u8>> {
        if frame.len() < 9 + 16 || frame[0] != FRAME_SECURE {
            return Err(PairError::Crypto);
        }
        let counter = u64::from_be_bytes(frame[1..9].try_into().unwrap());
        if counter <= self.recv_counter {
            return Err(PairError::Crypto);
        }
        let nonce = gcm_nonce(self.recv_dir, counter);
        let plain = self
            .cipher
            .decrypt((&nonce).into(), Payload { msg: &frame[9..], aad: &frame[..9] })
            .map_err(|_| PairError::Crypto)?;
        self.recv_counter = counter;
        Ok(plain)
    }
}

// --- host side --------------------------------------------------------------

/// Moves whole message frames to and from the device.
pub trait ControlChannel: Send {
    fn send(&mut self, frame: Vec<u8>) -> impl Future<Output = PairResult<()>> + Send;
    fn recv(&mut self, timeout: Duration) -> impl Future<Output = PairResult<Vec<u8>>> + Send;
}

#[derive(Debug, Clone, PartialEq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct WifiResult {
    pub ok: bool,
    pub ip: Option<String>,
}

pub struct Session<C> {
    chan: C,
    secure: SecureChannel,
    pub device_pk: PublicKeyBytes,
    pub device_id: String,
    /// The device already has this host as its peer (no code needed).
    pub known: bool,
    /// Six-digit code to compare; None for a known host.
    pub code: Option<String>,
}

const STEP_TIMEOUT: Duration = Duration::from_secs(15);

impl<C: ControlChannel> Session<C> {
    /// Runs the hello / hello / reveal exchange.
    pub async fn open(mut chan: C, host_secret: &SecretKey) -> PairResult<Self> {
        let host_pk = public_bytes(host_secret);
        let host_nonce: [u8; 16] = random();
        let commit = commitment(&host_pk, &host_nonce);
        chan.send(plain_frame(&encode(vec![
            ("t", Value::Text("hello".into())),
            ("pk", Value::Bytes(host_pk.to_vec())),
            ("c", Value::Bytes(commit.to_vec())),
        ])))
        .await?;

        let reply = chan.recv(STEP_TIMEOUT).await?;
        let msg = parse_plain(&reply)?;
        if msg.kind == "error" {
            return Err(PairError::Device(msg.text("e").unwrap_or_default()));
        }
        if msg.kind != "hello" {
            return Err(PairError::Protocol(msg.kind));
        }
        let device_pk: PublicKeyBytes = msg.fixed("pk")?;
        let device_nonce: [u8; 16] = msg.fixed("n")?;
        let known = msg.flag("known");

        chan.send(plain_frame(&encode(vec![
            ("t", Value::Text("reveal".into())),
            ("n", Value::Bytes(host_nonce.to_vec())),
        ])))
        .await?;

        let shared = ecdh(host_secret, &device_pk)?;
        let key = session_key(&shared, &host_pk, &device_pk, &host_nonce, &device_nonce);
        let mut session = Session {
            chan,
            secure: SecureChannel::new(&key, true),
            device_id: key_id(&device_pk),
            device_pk,
            known,
            code: (!known).then(|| comparison_code(&host_pk, &device_pk, &host_nonce, &device_nonce)),
        };
        if known {
            // Proves the device holds its key and still has us as its peer.
            session.expect("session", STEP_TIMEOUT).await?;
        }
        Ok(session)
    }

    async fn send_secure(&mut self, entries: Vec<(&str, Value)>) -> PairResult<()> {
        let frame = self.secure.seal(&encode(entries));
        self.chan.send(frame).await
    }

    /// Waits for an encrypted message of type `kind`. A plain `error` from
    /// the device (cancelled, timed out) ends the wait.
    async fn expect(&mut self, kind: &str, timeout: Duration) -> PairResult<Message> {
        let frame = self.chan.recv(timeout).await?;
        let msg = match frame.first() {
            Some(&FRAME_SECURE) => Message::parse(&self.secure.open(&frame)?)?,
            Some(&FRAME_PLAIN) => parse_plain(&frame)?,
            _ => return Err(PairError::Protocol("empty frame".into())),
        };
        if msg.kind == "error" {
            return Err(PairError::Device(msg.text("e").unwrap_or_default()));
        }
        if msg.kind != kind || frame[0] != FRAME_SECURE {
            return Err(PairError::Protocol(format!("expected {kind}, got {}", msg.kind)));
        }
        Ok(msg)
    }

    /// Waits until the user pressed Enter on the device.
    pub async fn wait_confirmed(&mut self, timeout: Duration) -> PairResult<()> {
        self.expect("confirmed", timeout).await.map(|_| ())
    }

    /// WiFi networks the device saw when pairing mode started.
    pub async fn networks(&mut self) -> PairResult<Vec<String>> {
        Ok(self.expect("networks", STEP_TIMEOUT).await?.texts("list"))
    }

    /// New pairing: hands the device our identity and WiFi credentials.
    pub async fn send_pair(&mut self, host_name: &str, host_cert_der: &[u8], ssid: &str, password: &str) -> PairResult<()> {
        self.send_secure(vec![
            ("t", Value::Text("pair".into())),
            ("name", Value::Text(host_name.into())),
            ("cert", Value::Bytes(host_cert_der.to_vec())),
            ("ssid", Value::Text(ssid.into())),
            ("pass", Value::Text(password.into())),
        ])
        .await
    }

    /// Known host: change WiFi without re-pairing.
    pub async fn send_set_wifi(&mut self, ssid: &str, password: &str) -> PairResult<()> {
        self.send_secure(vec![
            ("t", Value::Text("set_wifi".into())),
            ("ssid", Value::Text(ssid.into())),
            ("pass", Value::Text(password.into())),
        ])
        .await
    }

    /// The device tries the network once and reports.
    pub async fn wifi_result(&mut self) -> PairResult<WifiResult> {
        let msg = self.expect("wifi", Duration::from_secs(40)).await?;
        Ok(WifiResult { ok: msg.flag("ok"), ip: msg.text("ip").filter(|s| !s.is_empty()) })
    }

    pub fn into_channel(self) -> C {
        self.chan
    }
}

fn parse_plain(frame: &[u8]) -> PairResult<Message> {
    match frame.split_first() {
        Some((&FRAME_PLAIN, body)) => Message::parse(body),
        _ => Err(PairError::Protocol("expected a plain frame".into())),
    }
}

// --- reference device (tests) -----------------------------------------------

#[cfg(test)]
pub(crate) mod fake {
    //! The device side of the protocol in Rust. The firmware implements the
    //! same steps in `firmware/src/app/pairing_flow.cpp`; shared vectors in
    //! both test suites keep the primitives in agreement.
    use super::*;
    use tokio::sync::mpsc;

    pub struct ChannelEnd {
        pub tx: mpsc::UnboundedSender<Vec<u8>>,
        pub rx: mpsc::UnboundedReceiver<Vec<u8>>,
    }

    impl ControlChannel for ChannelEnd {
        async fn send(&mut self, frame: Vec<u8>) -> PairResult<()> {
            self.tx.send(frame).map_err(|_| PairError::Transport("closed".into()))
        }
        async fn recv(&mut self, timeout: Duration) -> PairResult<Vec<u8>> {
            match tokio::time::timeout(timeout, self.rx.recv()).await {
                Ok(Some(f)) => Ok(f),
                Ok(None) => Err(PairError::Transport("closed".into())),
                Err(_) => Err(PairError::Timeout),
            }
        }
    }

    pub fn pipe() -> (ChannelEnd, ChannelEnd) {
        let (a_tx, a_rx) = mpsc::unbounded_channel();
        let (b_tx, b_rx) = mpsc::unbounded_channel();
        (ChannelEnd { tx: a_tx, rx: b_rx }, ChannelEnd { tx: b_tx, rx: a_rx })
    }

    #[derive(Clone)]
    pub struct DeviceBehavior {
        pub peer: Option<PublicKeyBytes>,
        pub user_confirms: bool,
        pub networks: Vec<String>,
        pub wifi_ok: bool,
        /// Corrupt the device's half of the code (a MITM's effect).
        pub tamper_nonce: bool,
    }

    #[derive(Debug, Default)]
    pub struct DeviceOutcome {
        pub code: Option<String>,
        pub stored_peer: Option<PublicKeyBytes>,
        pub stored_name: Option<String>,
        pub stored_ssid: Option<String>,
        pub stored_pass: Option<String>,
        pub cert_len: usize,
    }

    pub async fn run_device(mut chan: ChannelEnd, secret: SecretKey, b: DeviceBehavior) -> PairResult<DeviceOutcome> {
        let mut out = DeviceOutcome::default();
        let device_pk = public_bytes(&secret);
        let hello = parse_plain(&chan.recv(STEP_TIMEOUT).await?)?;
        let host_pk: PublicKeyBytes = hello.fixed("pk")?;
        let commit: [u8; 32] = hello.fixed("c")?;
        let known = b.peer == Some(host_pk);
        let device_nonce: [u8; 16] = random();
        chan.send(plain_frame(&encode(vec![
            ("t", Value::Text("hello".into())),
            ("pk", Value::Bytes(device_pk.to_vec())),
            ("n", Value::Bytes(device_nonce.to_vec())),
            ("known", Value::Bool(known)),
        ])))
        .await?;
        let reveal = parse_plain(&chan.recv(STEP_TIMEOUT).await?)?;
        let host_nonce: [u8; 16] = reveal.fixed("n")?;
        if commitment(&host_pk, &host_nonce) != commit {
            chan.send(plain_frame(&encode(vec![("t", Value::Text("error".into())), ("e", Value::Text("commitment".into()))])))
                .await?;
            return Err(PairError::Crypto);
        }
        let shared = ecdh(&secret, &host_pk)?;
        let key = session_key(&shared, &host_pk, &device_pk, &host_nonce, &device_nonce);
        let mut sc = SecureChannel::new(&key, false);
        let shown_nonce = if b.tamper_nonce { [0xEE; 16] } else { device_nonce };
        out.code = Some(comparison_code(&host_pk, &device_pk, &host_nonce, &shown_nonce));

        if known {
            chan.send(sc.seal(&encode(vec![("t", Value::Text("session".into()))]))).await?;
        } else if b.user_confirms {
            chan.send(sc.seal(&encode(vec![("t", Value::Text("confirmed".into()))]))).await?;
        } else {
            chan.send(plain_frame(&encode(vec![("t", Value::Text("error".into())), ("e", Value::Text("cancelled".into()))])))
                .await?;
            return Ok(out);
        }
        let list = b.networks.iter().map(|s| Value::Text(s.clone())).collect();
        chan.send(sc.seal(&encode(vec![("t", Value::Text("networks".into())), ("list", Value::Array(list))])))
            .await?;

        let msg = Message::parse(&sc.open(&chan.recv(STEP_TIMEOUT).await?)?)?;
        match msg.kind.as_str() {
            "pair" if !known => {
                out.stored_peer = Some(host_pk);
                out.stored_name = msg.text("name");
                out.cert_len = msg.bytes("cert").map_or(0, |c| c.len());
            }
            "set_wifi" if known => {}
            other => return Err(PairError::Protocol(other.into())),
        }
        out.stored_ssid = msg.text("ssid");
        out.stored_pass = msg.text("pass");
        let ip = if b.wifi_ok { "192.168.1.50" } else { "" };
        chan.send(sc.seal(&encode(vec![
            ("t", Value::Text("wifi".into())),
            ("ok", Value::Bool(b.wifi_ok)),
            ("ip", Value::Text(ip.into())),
        ])))
        .await?;
        Ok(out)
    }
}

#[cfg(test)]
mod tests {
    use super::fake::*;
    use super::*;

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    /// Fixed inputs shared with firmware/test/test_pairing. If these change,
    /// update the firmware test too.
    fn vector_inputs() -> ([u8; 32], PublicKeyBytes, PublicKeyBytes, [u8; 16], [u8; 16]) {
        let shared = [0x11u8; 32];
        let mut host_pk = [0x22u8; 65];
        host_pk[0] = 0x04;
        let mut device_pk = [0x33u8; 65];
        device_pk[0] = 0x04;
        (shared, host_pk, device_pk, [0x44u8; 16], [0x55u8; 16])
    }

    #[test]
    fn shared_vectors_with_firmware() {
        // If any of these change, update firmware/test/test_pairing to match.
        let (shared, hpk, dpk, hn, dn) = vector_inputs();
        assert_eq!(hex(&commitment(&hpk, &hn)), "e13cafbba9ccc718f9ec5a7a3245fc5039e5d4de97f76b5551474740e8fcafee");
        assert_eq!(
            hex(&session_key(&shared, &hpk, &dpk, &hn, &dn)),
            "4a51c5239ed6d613897211f18e3c813c4d70a0f1e2b743716aa8fb97b686f7b8"
        );
        assert_eq!(comparison_code(&hpk, &dpk, &hn, &dn), "581958");
        assert_eq!(key_id(&hpk), "6fb33342010e11a7");
        // CBOR the firmware encoder must produce byte for byte.
        assert_eq!(hex(&encode(vec![("t", Value::Text("confirmed".into()))])), "a1617469636f6e6669726d6564");
        assert_eq!(
            hex(&encode(vec![("t", Value::Text("wifi".into())), ("ok", Value::Bool(true)), ("ip", Value::Text("".into()))])),
            "a361746477696669626f6bf562697060"
        );
    }

    #[tokio::test]
    async fn new_pairing_matches_codes_and_provisions_wifi() {
        let host = SecretKey::random(&mut OsRng);
        let device = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        let behavior = DeviceBehavior {
            peer: None,
            user_confirms: true,
            networks: vec!["Home".into(), "Guest".into()],
            wifi_ok: true,
            tamper_nonce: false,
        };
        let dev = tokio::spawn(run_device(device_end, device.clone(), behavior));

        let mut s = Session::open(host_end, &host).await.unwrap();
        assert!(!s.known);
        assert_eq!(s.device_id, key_id(&public_bytes(&device)));
        s.wait_confirmed(Duration::from_secs(5)).await.unwrap();
        assert_eq!(s.networks().await.unwrap(), vec!["Home", "Guest"]);
        s.send_pair("alice", &[1, 2, 3], "Home", "pw").await.unwrap();
        let wifi = s.wifi_result().await.unwrap();
        assert!(wifi.ok);
        assert_eq!(wifi.ip.as_deref(), Some("192.168.1.50"));

        let out = dev.await.unwrap().unwrap();
        assert_eq!(out.code, s.code, "both screens show the same code");
        assert_eq!(out.stored_peer, Some(public_bytes(&host)));
        assert_eq!(out.stored_name.as_deref(), Some("alice"));
        assert_eq!(out.stored_ssid.as_deref(), Some("Home"));
        assert_eq!(out.stored_pass.as_deref(), Some("pw"));
        assert_eq!(out.cert_len, 3);
    }

    #[tokio::test]
    async fn known_host_skips_the_code_and_changes_wifi() {
        let host = SecretKey::random(&mut OsRng);
        let device = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        let behavior = DeviceBehavior {
            peer: Some(public_bytes(&host)),
            user_confirms: false,
            networks: vec![],
            wifi_ok: false,
            tamper_nonce: false,
        };
        let dev = tokio::spawn(run_device(device_end, device, behavior));
        let mut s = Session::open(host_end, &host).await.unwrap();
        assert!(s.known);
        assert!(s.code.is_none());
        s.networks().await.unwrap();
        s.send_set_wifi("Office", "pw2").await.unwrap();
        assert!(!s.wifi_result().await.unwrap().ok);
        assert_eq!(dev.await.unwrap().unwrap().stored_ssid.as_deref(), Some("Office"));
    }

    #[tokio::test]
    async fn cancel_on_device_is_reported() {
        let host = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        let behavior =
            DeviceBehavior { peer: None, user_confirms: false, networks: vec![], wifi_ok: false, tamper_nonce: false };
        tokio::spawn(run_device(device_end, SecretKey::random(&mut OsRng), behavior));
        let mut s = Session::open(host_end, &host).await.unwrap();
        assert_eq!(
            s.wait_confirmed(Duration::from_secs(5)).await.unwrap_err(),
            PairError::Device("cancelled".into())
        );
    }

    #[tokio::test]
    async fn tampered_exchange_shows_different_codes() {
        let host = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        let behavior =
            DeviceBehavior { peer: None, user_confirms: true, networks: vec![], wifi_ok: true, tamper_nonce: true };
        let dev = tokio::spawn(run_device(device_end, SecretKey::random(&mut OsRng), behavior));
        let mut s = Session::open(host_end, &host).await.unwrap();
        s.wait_confirmed(Duration::from_secs(5)).await.unwrap();
        s.networks().await.unwrap();
        s.send_pair("a", &[], "x", "y").await.unwrap();
        s.wifi_result().await.unwrap();
        assert_ne!(dev.await.unwrap().unwrap().code, s.code);
    }

    #[test]
    fn secure_channel_round_trips_and_rejects_replay_and_tamper() {
        let key = [7u8; 32];
        let mut host = SecureChannel::new(&key, true);
        let mut device = SecureChannel::new(&key, false);
        let f1 = host.seal(b"one");
        let f2 = host.seal(b"two");
        assert_eq!(device.open(&f1).unwrap(), b"one");
        assert_eq!(device.open(&f2).unwrap(), b"two");
        assert_eq!(device.open(&f1).unwrap_err(), PairError::Crypto, "replay");
        let mut bad = host.seal(b"three");
        let last = bad.len() - 1;
        bad[last] ^= 1;
        assert_eq!(device.open(&bad).unwrap_err(), PairError::Crypto, "tamper");
        // A frame sealed in the device direction cannot be fed back to it.
        let echo = device.seal(b"x");
        let mut device2 = SecureChannel::new(&key, false);
        assert!(device2.open(&echo).is_err());
    }

    #[test]
    fn fragments_reassemble() {
        let msg: Vec<u8> = (0..500u32).map(|i| i as u8).collect();
        let frags = fragment(&msg, 20);
        assert!(frags.iter().all(|f| f.len() <= 20));
        assert_eq!(frags.iter().filter(|f| f[0] == 1).count(), 1);
        let mut r = Reassembler::default();
        let mut out = None;
        for f in &frags {
            out = r.push(f).unwrap();
        }
        assert_eq!(out.unwrap(), msg);
        assert_eq!(fragment(&[], 20), vec![vec![1u8]]);
        let mut r = Reassembler::default();
        let big = vec![0u8; 200];
        let mut err = None;
        for _ in 0..20 {
            if let Err(e) = r.push(&[&[0u8][..], &big].concat()) {
                err = Some(e);
                break;
            }
        }
        assert!(err.is_some(), "oversized messages are rejected");
    }
}
