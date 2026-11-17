//! BLE fallback transfer (milestone 6): the `/v1` protocol tunnelled through
//! the encrypted pairing channel, for when the device cannot reach WiFi.
//!
//! The paired desktop opens a known-host session (hello / hello / reveal,
//! then `session`) and sends one request at a time:
//!
//! - host: `{t:"req", m, p, since?, range?, body?}` (method, path, and the
//!   query, Range header and JSON body the HTTP request would carry)
//! - device: `{t:"res", s, ct, cr}`, then `{t:"body", d}` messages, then
//!   `{t:"end"}`
//!
//! Wire details: docs/ble-protocol.md, "Sync fallback". [`BleDevice`]
//! implements [`Device`], so the sync engine runs unchanged over it.

use std::{path::Path, time::Duration};

use ciborium::Value;
use tokio::{io::AsyncWriteExt, sync::Mutex};

use crate::{
    device::{Device, DeviceError, DeviceResult},
    pairing::{ControlChannel, PairError, Session},
    protocol::{AckResult, ApiErrorBody, DeleteResult, DeviceInfo, Manifest, Sha256Response},
};

/// Hashing a long recording on the device happens before `res` arrives.
const HEAD_TIMEOUT: Duration = Duration::from_secs(120);
/// Between body messages of one response.
const BODY_TIMEOUT: Duration = Duration::from_secs(20);

/// Rough BLE throughput, for the time estimate shown before a sync.
pub const ESTIMATED_BYTES_PER_SEC: u64 = 30 * 1024;

struct Link<C> {
    session: Session<C>,
    /// Set when a response broke off: later messages could belong to it,
    /// so the link is not used again.
    broken: bool,
}

pub struct BleDevice<C> {
    link: Mutex<Link<C>>,
}

struct Head {
    status: u16,
}

fn transport(e: PairError) -> DeviceError {
    match e {
        PairError::Device(code) if code == "not_paired" => DeviceError::Api {
            status: 403,
            code: "not_paired".into(),
            message: "the device is paired with another desktop".into(),
        },
        other => DeviceError::Unreachable(other.to_string()),
    }
}

impl<C: ControlChannel> BleDevice<C> {
    pub fn new(session: Session<C>) -> Self {
        Self { link: Mutex::new(Link { session, broken: false }) }
    }

    pub fn into_session(self) -> Session<C> {
        self.link.into_inner().session
    }

    /// Sends a request and streams the body to `sink`. Errors with status
    /// >= 400 come back as [`DeviceError::Api`] (their body is read here).
    async fn request(
        &self,
        method: &str,
        path: &str,
        since: Option<u64>,
        range: Option<String>,
        body: Option<String>,
        sink: &mut (dyn FnMut(&[u8]) -> DeviceResult<()> + Send),
    ) -> DeviceResult<Head> {
        let mut link = self.link.lock().await;
        if link.broken {
            return Err(DeviceError::Unreachable("the Bluetooth link was interrupted".into()));
        }
        let mut entries = vec![
            ("t", Value::Text("req".into())),
            ("m", Value::Text(method.into())),
            ("p", Value::Text(format!("/v1{path}"))),
        ];
        if let Some(since) = since {
            entries.push(("since", Value::Text(since.to_string())));
        }
        if let Some(range) = range {
            entries.push(("range", Value::Text(range)));
        }
        if let Some(body) = body {
            entries.push(("body", Value::Text(body)));
        }
        link.session.send_message(entries).await.map_err(transport)?;

        // Any failure from here on leaves the stream mid-response.
        link.broken = true;
        let head = link.session.next_message(HEAD_TIMEOUT).await.map_err(transport)?;
        if head.kind != "res" {
            return Err(DeviceError::Protocol(format!("expected res, got {}", head.kind)));
        }
        let status = head
            .uint("s")
            .and_then(|s| u16::try_from(s).ok())
            .ok_or_else(|| DeviceError::Protocol("response without status".into()))?;
        let mut error_body = Vec::new();
        loop {
            let msg = match link.session.next_message(BODY_TIMEOUT).await {
                Ok(msg) => msg,
                Err(e @ (PairError::Timeout | PairError::Transport(_))) => {
                    return Err(DeviceError::Interrupted(e.to_string()));
                }
                Err(e) => return Err(transport(e)),
            };
            match msg.kind.as_str() {
                "body" => {
                    let data = msg.bytes("d").unwrap_or_default();
                    if status >= 400 {
                        error_body.extend_from_slice(&data);
                    } else {
                        sink(&data)?;
                    }
                }
                "end" => break,
                other => return Err(DeviceError::Protocol(format!("unexpected {other} in a response"))),
            }
        }
        link.broken = false;

        if status >= 400 {
            let text = String::from_utf8_lossy(&error_body).into_owned();
            return Err(match serde_json::from_str::<ApiErrorBody>(&text) {
                Ok(body) => DeviceError::Api { status, code: body.code, message: body.message },
                Err(_) => DeviceError::Api { status, code: "http".into(), message: text },
            });
        }
        Ok(Head { status })
    }

    async fn json<T: serde::de::DeserializeOwned>(
        &self,
        method: &str,
        path: &str,
        since: Option<u64>,
        body: Option<serde_json::Value>,
    ) -> DeviceResult<T> {
        let mut bytes = Vec::new();
        let mut sink = |d: &[u8]| -> DeviceResult<()> {
            bytes.extend_from_slice(d);
            Ok(())
        };
        self.request(method, path, since, None, body.map(|b| b.to_string()), &mut sink).await?;
        serde_json::from_slice(&bytes).map_err(|e| DeviceError::Protocol(e.to_string()))
    }

    async fn empty(&self, method: &str, path: &str, body: Option<serde_json::Value>) -> DeviceResult<()> {
        let mut sink = |_: &[u8]| -> DeviceResult<()> { Ok(()) };
        self.request(method, path, None, None, body.map(|b| b.to_string()), &mut sink).await?;
        Ok(())
    }
}

impl<C: ControlChannel> Device for BleDevice<C> {
    async fn info(&self) -> DeviceResult<DeviceInfo> {
        self.json("GET", "/info", None, None).await
    }

    async fn set_time(&self, unix_ms: i64, tz_offset_min: i32) -> DeviceResult<()> {
        let body = serde_json::json!({ "unix_ms": unix_ms, "tz_offset_min": tz_offset_min });
        self.empty("POST", "/time", Some(body)).await
    }

    async fn manifest(&self, since: Option<u64>) -> DeviceResult<Manifest> {
        self.json("GET", "/manifest", since, None).await
    }

    async fn download(&self, id: &str, part: &Path, progress: &(dyn Fn(u64) + Send + Sync)) -> DeviceResult<u64> {
        let offset = match tokio::fs::metadata(part).await {
            Ok(meta) => meta.len(),
            Err(_) => 0,
        };
        let range = (offset > 0).then(|| format!("bytes={offset}-"));

        // Body messages arrive inside request(); buffer them and write in
        // batches so the BLE session lock is not held across file I/O.
        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();
        let path = format!("/recordings/{id}");
        // Owns the sender, so the writer sees the channel close when the
        // response ends.
        let request = async move {
            let mut sink = move |d: &[u8]| -> DeviceResult<()> {
                tx.send(d.to_vec()).map_err(|_| DeviceError::Io("writer stopped".into()))
            };
            self.request("GET", &path, None, range, None, &mut sink).await
        };

        let writer = async {
            let mut file: Option<tokio::fs::File> = None;
            let mut written = 0u64;
            while let Some(chunk) = rx.recv().await {
                if file.is_none() {
                    // The first chunk decides: appended to a partial file
                    // when we asked for a range, a fresh file otherwise.
                    file = Some(if offset > 0 {
                        written = offset;
                        tokio::fs::OpenOptions::new().append(true).open(part).await?
                    } else {
                        tokio::fs::File::create(part).await?
                    });
                }
                file.as_mut().unwrap().write_all(&chunk).await?;
                written += chunk.len() as u64;
                progress(written);
            }
            if let Some(f) = file.as_mut() {
                f.flush().await?;
                f.sync_all().await?;
            }
            Ok::<u64, DeviceError>(written)
        };

        let (head, written) = tokio::join!(request, writer);
        let written = written?;
        match head {
            Ok(Head { status: 206 }) => Ok(written),
            Ok(_) if offset > 0 => {
                // Full body although we asked for a range: it was written
                // after the old part, so start over next time.
                let _ = tokio::fs::remove_file(part).await;
                Err(DeviceError::Protocol("device ignored the range".into()))
            }
            Ok(_) => Ok(written),
            // The part is already complete: the device has nothing past it.
            Err(DeviceError::Api { status: 416, .. }) if offset > 0 => Ok(offset),
            Err(e) => Err(e),
        }
    }

    async fn sha256(&self, id: &str) -> DeviceResult<String> {
        let r: Sha256Response = self.json("GET", &format!("/recordings/{id}/sha256"), None, None).await?;
        Ok(r.sha256)
    }

    async fn ack(&self, id: &str, sha256: &str) -> DeviceResult<AckResult> {
        let body = serde_json::json!({ "sha256": sha256 });
        self.json("POST", &format!("/recordings/{id}/ack"), None, Some(body)).await
    }

    async fn delete(&self, id: &str) -> DeviceResult<bool> {
        let r: DeleteResult = self.json("DELETE", &format!("/recordings/{id}"), None, None).await?;
        Ok(r.deleted)
    }

    async fn done(&self) -> DeviceResult<()> {
        self.empty("POST", "/done", None).await
    }
}

/// Minutes the transfer will roughly take, for the UI.
pub fn estimate_secs(bytes: u64) -> u64 {
    bytes.div_ceil(ESTIMATED_BYTES_PER_SEC)
}

#[cfg(test)]
mod tests {
    use std::{
        collections::BTreeMap,
        sync::{Arc, Mutex as StdMutex},
    };

    use p256::SecretKey;
    use rand_core::OsRng;
    use sha2::{Digest, Sha256};

    use super::*;
    use crate::{
        db::Db,
        pairing::{
            Message, PublicKeyBytes, SecureChannel, commitment, ecdh, encode,
            fake::{ChannelEnd, pipe},
            plain_frame, public_bytes, session_key,
        },
        sync::{SyncTarget, sync_device},
    };

    #[derive(Clone)]
    struct Rec {
        name: String,
        data: Vec<u8>,
        acked: bool,
    }

    /// The device side in Rust: the known-host handshake, then /v1 requests
    /// framed as the firmware does (app/ble_sync_flow.cpp), with body
    /// messages of 1536 bytes.
    async fn serve(
        mut chan: ChannelEnd,
        secret: SecretKey,
        peer: PublicKeyBytes,
        recs: Arc<StdMutex<BTreeMap<String, Rec>>>,
        drop_after_bytes: Option<usize>,
    ) {
        let device_pk = public_bytes(&secret);
        let hello = Message::parse(&chan.recv(Duration::from_secs(5)).await.unwrap()[1..]).unwrap();
        let host_pk: PublicKeyBytes = hello.fixed("pk").unwrap();
        let commit: [u8; 32] = hello.fixed("c").unwrap();
        if host_pk != peer {
            chan.send(plain_frame(&encode(vec![
                ("t", Value::Text("error".into())),
                ("e", Value::Text("not_paired".into())),
            ])))
            .await
            .unwrap();
            return;
        }
        let device_nonce = [7u8; 16];
        chan.send(plain_frame(&encode(vec![
            ("t", Value::Text("hello".into())),
            ("pk", Value::Bytes(device_pk.to_vec())),
            ("n", Value::Bytes(device_nonce.to_vec())),
            ("known", Value::Bool(true)),
        ])))
        .await
        .unwrap();
        let reveal = Message::parse(&chan.recv(Duration::from_secs(5)).await.unwrap()[1..]).unwrap();
        let host_nonce: [u8; 16] = reveal.fixed("n").unwrap();
        assert_eq!(commitment(&host_pk, &host_nonce), commit);
        let shared = ecdh(&secret, &host_pk).unwrap();
        let key = session_key(&shared, &host_pk, &device_pk, &host_nonce, &device_nonce);
        let mut sc = SecureChannel::new(&key, false);
        chan.send(sc.seal(&encode(vec![("t", Value::Text("session".into()))]))).await.unwrap();

        let mut sent_body_bytes = 0usize;
        while let Ok(frame) = chan.recv(Duration::from_secs(5)).await {
            let req = Message::parse(&sc.open(&frame).unwrap()).unwrap();
            assert_eq!(req.kind, "req");
            let method = req.text("m").unwrap();
            let path = req.text("p").unwrap();
            let body = req.text("body").unwrap_or_default();
            let (status, mut out, done) = route(&method, &path, req.text("range"), &body, &recs);
            chan.send(sc.seal(&encode(vec![
                ("t", Value::Text("res".into())),
                ("s", Value::Integer(status.into())),
                ("ct", Value::Text("application/json".into())),
                ("cr", Value::Text(String::new())),
            ])))
            .await
            .unwrap();
            while !out.is_empty() {
                let take = out.len().min(1536);
                let chunk: Vec<u8> = out.drain(..take).collect();
                sent_body_bytes += take;
                chan.send(sc.seal(&encode(vec![("t", Value::Text("body".into())), ("d", Value::Bytes(chunk))])))
                    .await
                    .unwrap();
                if drop_after_bytes.is_some_and(|limit| sent_body_bytes >= limit) {
                    return; // link lost mid-body
                }
            }
            chan.send(sc.seal(&encode(vec![("t", Value::Text("end".into()))]))).await.unwrap();
            if done {
                return;
            }
        }
    }

    fn sha_hex(data: &[u8]) -> String {
        Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
    }

    fn route(
        method: &str,
        path: &str,
        range: Option<String>,
        body: &str,
        recs: &StdMutex<BTreeMap<String, Rec>>,
    ) -> (u16, Vec<u8>, bool) {
        let mut recs = recs.lock().unwrap();
        let json = |v: serde_json::Value| v.to_string().into_bytes();
        let parts: Vec<&str> = path.trim_start_matches("/v1/").split('/').collect();
        match (method, parts.as_slice()) {
            ("GET", ["info"]) => (
                200,
                json(serde_json::json!({"device_id": "0123456789abcdef", "firmware": "test", "battery_pct": 80,
                    "free_bytes": 1, "unsynced": recs.values().filter(|r| !r.acked).count(), "clock_set": true})),
                false,
            ),
            ("POST", ["time"]) => (200, b"{}".to_vec(), false),
            ("GET", ["manifest"]) => {
                let list: Vec<_> = recs
                    .iter()
                    .map(|(id, r)| serde_json::json!({"id": id, "name": r.name, "size": r.data.len(),
                        "created_ms": 0, "duration_ms": 0, "locked": false, "acked": r.acked}))
                    .collect();
                (200, json(serde_json::json!({"cursor": 5, "recordings": list})), false)
            }
            ("GET", ["recordings", id]) => match recs.get(*id) {
                Some(r) => {
                    let start = range
                        .and_then(|r| r.strip_prefix("bytes=")?.strip_suffix('-')?.parse::<usize>().ok())
                        .unwrap_or(0);
                    if start >= r.data.len() {
                        return (416, json(serde_json::json!({"code": "bad_range", "message": "x"})), false);
                    }
                    (if start > 0 { 206 } else { 200 }, r.data[start..].to_vec(), false)
                }
                None => (404, json(serde_json::json!({"code": "not_found", "message": "x"})), false),
            },
            ("GET", ["recordings", id, "sha256"]) => match recs.get(*id) {
                Some(r) => (200, json(serde_json::json!({"sha256": sha_hex(&r.data)})), false),
                None => (404, json(serde_json::json!({"code": "not_found", "message": "x"})), false),
            },
            ("POST", ["recordings", id, "ack"]) => {
                let claimed: serde_json::Value = serde_json::from_str(body).unwrap();
                let rec = recs.get_mut(*id).unwrap();
                if claimed["sha256"] != sha_hex(&rec.data) {
                    return (409, json(serde_json::json!({"code": "hash_mismatch", "message": "x"})), false);
                }
                rec.acked = true;
                (200, json(serde_json::json!({"acked": true, "deleted": false})), false)
            }
            ("POST", ["done"]) => (204, Vec::new(), true),
            _ => (404, json(serde_json::json!({"code": "not_found", "message": "x"})), false),
        }
    }

    fn recordings() -> Arc<StdMutex<BTreeMap<String, Rec>>> {
        let mut map = BTreeMap::new();
        let big: Vec<u8> = (0..50_000u32).map(|i| (i % 251) as u8).collect();
        map.insert("a".repeat(32), Rec { name: "REC_A.WAV".into(), data: big, acked: false });
        map.insert("b".repeat(32), Rec { name: "REC_B.WAV".into(), data: b"short".to_vec(), acked: false });
        Arc::new(StdMutex::new(map))
    }

    #[tokio::test]
    async fn syncs_over_the_ble_tunnel() {
        let host = SecretKey::random(&mut OsRng);
        let device = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        let recs = recordings();
        let server = tokio::spawn(serve(device_end, device, public_bytes(&host), recs.clone(), None));

        let session = Session::open(host_end, &host).await.unwrap();
        assert!(session.known);
        let ble = BleDevice::new(session);

        let dir = tempfile::tempdir().unwrap();
        let db = std::sync::Mutex::new(Db::open_in_memory().unwrap());
        let user = db.lock().unwrap().create_user("u", "hash", dir.path().to_str().unwrap(), 0).unwrap();
        let target = SyncTarget {
            db: &db,
            user_id: user.id,
            library: dir.path().to_path_buf(),
            address: None,
            expected_device_id: Some("0123456789abcdef".into()),
            now_ms: || 1_700_000_000_000,
            tz_offset_min: 0,
        };
        let report = sync_device(&ble, &target, &|_| {}).await.unwrap();
        assert_eq!(report.downloaded, 2);
        assert!(report.failed.is_empty());
        assert!(recs.lock().unwrap().values().all(|r| r.acked));
        server.await.unwrap();
    }

    #[tokio::test]
    async fn api_errors_and_resume_over_ble() {
        let host = SecretKey::random(&mut OsRng);
        let device = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        let recs = recordings();
        tokio::spawn(serve(device_end, device, public_bytes(&host), recs.clone(), None));
        let ble = BleDevice::new(Session::open(host_end, &host).await.unwrap());

        let missing = ble.sha256(&"c".repeat(32)).await;
        assert!(matches!(missing, Err(DeviceError::Api { status: 404, ref code, .. }) if code == "not_found"));

        // A partial file resumes from its length with a range request.
        let dir = tempfile::tempdir().unwrap();
        let part = dir.path().join("a.part");
        let full = recs.lock().unwrap()[&"a".repeat(32)].data.clone();
        tokio::fs::write(&part, &full[..20_000]).await.unwrap();
        let len = ble.download(&"a".repeat(32), &part, &|_| {}).await.unwrap();
        assert_eq!(len, full.len() as u64);
        assert_eq!(tokio::fs::read(&part).await.unwrap(), full);
        // Complete already: the device answers 416 and the part stands.
        assert_eq!(ble.download(&"a".repeat(32), &part, &|_| {}).await.unwrap(), full.len() as u64);
    }

    #[tokio::test]
    async fn a_dropped_link_interrupts_and_stays_down() {
        let host = SecretKey::random(&mut OsRng);
        let device = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        tokio::spawn(serve(device_end, device, public_bytes(&host), recordings(), Some(10_000)));
        let ble = BleDevice::new(Session::open(host_end, &host).await.unwrap());

        let dir = tempfile::tempdir().unwrap();
        let part = dir.path().join("a.part");
        let first = ble.download(&"a".repeat(32), &part, &|_| {}).await;
        assert!(matches!(first, Err(DeviceError::Interrupted(_))), "{first:?}");
        // What arrived is kept for a later resume.
        assert!(tokio::fs::metadata(&part).await.unwrap().len() >= 10_000);
        assert!(matches!(ble.info().await, Err(DeviceError::Unreachable(_))));
    }

    #[tokio::test]
    async fn another_desktop_is_refused() {
        let host = SecretKey::random(&mut OsRng);
        let stranger = SecretKey::random(&mut OsRng);
        let device = SecretKey::random(&mut OsRng);
        let (host_end, device_end) = pipe();
        tokio::spawn(serve(device_end, device, public_bytes(&host), recordings(), None));
        let err = Session::open(host_end, &stranger).await.err().unwrap();
        assert_eq!(err, PairError::Device("not_paired".into()));
    }

    #[test]
    fn estimates_transfer_time() {
        assert_eq!(estimate_secs(0), 0);
        assert_eq!(estimate_secs(ESTIMATED_BYTES_PER_SEC * 60), 60);
    }
}
