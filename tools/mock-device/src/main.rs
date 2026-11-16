//! Fake Cardputer ADV Recorder. Serves the /v1 sync protocol (plain HTTP, the
//! milestone 2/3 dev profile) from a folder of WAV files so the desktop app can
//! be developed without hardware. Mirrors docs/architecture.

use axum::{
    body::{Body, Bytes},
    extract::{Path, Query, State},
    http::{header, HeaderMap, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use clap::{Parser, ValueEnum};
use futures_util::StreamExt;
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    path::PathBuf,
    sync::{Arc, Mutex},
};

#[derive(Clone, Copy, ValueEnum, PartialEq, Debug)]
enum Fault {
    /// Behave correctly.
    None,
    /// Cut the connection halfway through every file download.
    DropMid,
    /// Report a wrong SHA-256 for every file.
    HashMismatch,
    /// Answer everything except /v1/info with 409 busy.
    Busy,
    /// Report 3% battery and refuse everything except /v1/info with 403.
    LowBattery,
}

#[derive(Parser)]
struct Args {
    /// Folder of .wav files that act as the device's SD card.
    #[arg(long, default_value = "./sdcard")]
    dir: PathBuf,
    #[arg(long, default_value_t = 8080)]
    port: u16,
    #[arg(long, value_enum, default_value_t = Fault::None)]
    fault: Fault,
    /// Delete a file on ack, like the "Delete after sync" setting. Files whose
    /// name contains LOCKED count as locked and are never deleted.
    #[arg(long)]
    delete_after_sync: bool,
    #[arg(long, default_value = "0123456789abcdef")]
    device_id: String,
    /// Announce _cprec._tcp.local (TXT id=<device id>) on loopback, like a
    /// device in sync mode, so the desktop's discovery worker finds it.
    #[arg(long)]
    mdns: bool,
}

struct Inner {
    acked: HashSet<String>,
    cursor: u64,
    signature: String,
    clock_set: bool,
}

#[derive(Clone)]
struct App {
    args: Arc<Args>,
    inner: Arc<Mutex<Inner>>,
}

struct Rec {
    id: String,
    name: String,
    path: PathBuf,
    size: u64,
    locked: bool,
}

fn err(status: StatusCode, code: &str, msg: &str) -> Response {
    (status, Json(json!({ "code": code, "message": msg }))).into_response()
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// Placeholder id: first 32 hex of SHA-256(NAME|size). Real firmware uses the
/// random 128-bit id from the cprc chunk; the format (32 hex chars) matches.
fn make_id(name: &str, size: u64) -> String {
    let h = Sha256::digest(format!("{}|{}", name.to_uppercase(), size));
    hex(&h)[..32].to_string()
}

impl App {
    /// Rescan the folder on every call so files can be added or removed live.
    /// The cursor is bumped whenever the listing or an ack changes.
    fn scan(&self) -> Vec<Rec> {
        let mut recs = vec![];
        if let Ok(rd) = std::fs::read_dir(&self.args.dir) {
            for e in rd.flatten() {
                let p = e.path();
                let is_wav = p.extension().map_or(false, |x| x.eq_ignore_ascii_case("wav"));
                if !is_wav {
                    continue;
                }
                let name = e.file_name().to_string_lossy().to_string();
                let size = e.metadata().map(|m| m.len()).unwrap_or(0);
                recs.push(Rec {
                    id: make_id(&name, size),
                    locked: name.to_uppercase().contains("LOCKED"),
                    name,
                    path: p,
                    size,
                });
            }
        }
        recs.sort_by(|a, b| b.name.cmp(&a.name));
        let sig: String = recs.iter().map(|r| format!("{}:{};", r.name, r.size)).collect();
        let mut i = self.inner.lock().unwrap();
        if sig != i.signature {
            i.signature = sig;
            i.cursor += 1;
        }
        recs
    }

    /// Fault gate for every endpoint except /v1/info.
    fn gate(&self) -> Option<Response> {
        match self.args.fault {
            Fault::Busy => Some(err(StatusCode::CONFLICT, "busy", "device is recording")),
            Fault::LowBattery => Some(err(
                StatusCode::FORBIDDEN,
                "low_battery",
                "battery below threshold",
            )),
            _ => None,
        }
    }
}

async fn info(State(a): State<App>) -> Response {
    let recs = a.scan();
    let i = a.inner.lock().unwrap();
    let unsynced = recs.iter().filter(|r| !i.acked.contains(&r.id)).count();
    Json(json!({
        "device_id": a.args.device_id,
        "firmware": "mock-0.1.0",
        "battery_pct": if a.args.fault == Fault::LowBattery { 3 } else { 87 },
        "free_bytes": 4_000_000_000u64,
        "unsynced": unsynced,
        "clock_set": i.clock_set,
    }))
    .into_response()
}

async fn time(State(a): State<App>, Json(b): Json<Value>) -> Response {
    if let Some(r) = a.gate() {
        return r;
    }
    if b.get("unix_ms").and_then(Value::as_u64).is_none() {
        return err(StatusCode::BAD_REQUEST, "bad_request", "unix_ms required");
    }
    a.inner.lock().unwrap().clock_set = true;
    Json(json!({})).into_response()
}

#[derive(Deserialize)]
struct Since {
    since: Option<u64>,
}

async fn manifest(State(a): State<App>, Query(q): Query<Since>) -> Response {
    if let Some(r) = a.gate() {
        return r;
    }
    let recs = a.scan();
    let i = a.inner.lock().unwrap();
    let list: Vec<Value> = if q.since.map_or(false, |s| s >= i.cursor) {
        vec![]
    } else {
        recs.iter()
            .map(|r| {
                json!({
                    "id": r.id, "name": r.name, "size": r.size,
                    "created_ms": 0,
                    "duration_ms": r.size.saturating_sub(44) / 32,
                    "locked": r.locked,
                    "acked": i.acked.contains(&r.id),
                })
            })
            .collect()
    };
    Json(json!({ "cursor": i.cursor, "recordings": list })).into_response()
}

fn find(a: &App, id: &str) -> Option<Rec> {
    a.scan().into_iter().find(|r| r.id == id)
}

fn parse_range(h: &HeaderMap, size: u64) -> Option<(u64, u64)> {
    let v = h.get(header::RANGE)?.to_str().ok()?.strip_prefix("bytes=")?;
    let (s, e) = v.split_once('-')?;
    let start: u64 = s.parse().ok()?;
    let end: u64 = if e.is_empty() { size.checked_sub(1)? } else { e.parse().ok()? };
    (start <= end && end < size).then_some((start, end))
}

async fn download(State(a): State<App>, Path(id): Path<String>, headers: HeaderMap) -> Response {
    if let Some(r) = a.gate() {
        return r;
    }
    let Some(rec) = find(&a, &id) else {
        return err(StatusCode::NOT_FOUND, "not_found", "no such recording");
    };
    let Ok(data) = std::fs::read(&rec.path) else {
        return err(StatusCode::INTERNAL_SERVER_ERROR, "sd_error", "read failed");
    };
    let size = data.len() as u64;
    let (status, start, end) = if headers.contains_key(header::RANGE) {
        match parse_range(&headers, size) {
            Some((s, e)) => (StatusCode::PARTIAL_CONTENT, s, e),
            None => return err(StatusCode::RANGE_NOT_SATISFIABLE, "bad_range", "bad Range"),
        }
    } else {
        (StatusCode::OK, 0, size.saturating_sub(1))
    };
    let slice = &data[start as usize..=end as usize];
    let declared = slice.len();
    let body = if a.args.fault == Fault::DropMid {
        // Send half, then fail, while Content-Length promises the full body.
        // The pause lets hyper flush the headers and the first half before
        // the error resets the connection; without it nothing is sent.
        let half = Bytes::copy_from_slice(&slice[..declared / 2]);
        let first = futures_util::stream::iter([Ok::<_, std::io::Error>(half)]);
        let fail = futures_util::stream::once(async {
            tokio::time::sleep(std::time::Duration::from_millis(200)).await;
            Err(std::io::Error::new(std::io::ErrorKind::ConnectionReset, "mock drop"))
        });
        Body::from_stream(first.chain(fail))
    } else {
        let chunks: Vec<Result<Bytes, std::io::Error>> =
            slice.chunks(32 * 1024).map(|c| Ok(Bytes::copy_from_slice(c))).collect();
        Body::from_stream(futures_util::stream::iter(chunks))
    };
    let mut resp = Response::builder()
        .status(status)
        .header(header::CONTENT_TYPE, "audio/wav")
        .header(header::ACCEPT_RANGES, "bytes")
        .header(header::CONTENT_LENGTH, declared);
    if status == StatusCode::PARTIAL_CONTENT {
        resp = resp.header(header::CONTENT_RANGE, format!("bytes {start}-{end}/{size}"));
    }
    resp.body(body).unwrap()
}

fn file_sha(rec: &Rec) -> Option<String> {
    std::fs::read(&rec.path).ok().map(|d| hex(&Sha256::digest(d)))
}

async fn sha(State(a): State<App>, Path(id): Path<String>) -> Response {
    if let Some(r) = a.gate() {
        return r;
    }
    let Some(rec) = find(&a, &id) else {
        return err(StatusCode::NOT_FOUND, "not_found", "no such recording");
    };
    let Some(mut h) = file_sha(&rec) else {
        return err(StatusCode::INTERNAL_SERVER_ERROR, "sd_error", "read failed");
    };
    if a.args.fault == Fault::HashMismatch {
        h = "0".repeat(64);
    }
    Json(json!({ "sha256": h })).into_response()
}

async fn ack(State(a): State<App>, Path(id): Path<String>, Json(b): Json<Value>) -> Response {
    if let Some(r) = a.gate() {
        return r;
    }
    let Some(rec) = find(&a, &id) else {
        return err(StatusCode::NOT_FOUND, "not_found", "no such recording");
    };
    // Like the device: the ack hash must match the file's real hash.
    if b.get("sha256").and_then(Value::as_str) != file_sha(&rec).as_deref() {
        return err(StatusCode::CONFLICT, "hash_mismatch", "ack hash differs");
    }
    let deleted =
        a.args.delete_after_sync && !rec.locked && std::fs::remove_file(&rec.path).is_ok();
    let mut i = a.inner.lock().unwrap();
    i.acked.insert(rec.id);
    i.cursor += 1;
    Json(json!({ "acked": true, "deleted": deleted })).into_response()
}

async fn delete_rec(State(a): State<App>, Path(id): Path<String>) -> Response {
    if let Some(r) = a.gate() {
        return r;
    }
    let Some(rec) = find(&a, &id) else {
        return err(StatusCode::NOT_FOUND, "not_found", "no such recording");
    };
    if rec.locked {
        return err(StatusCode::CONFLICT, "locked", "recording is locked");
    }
    let ok = std::fs::remove_file(&rec.path).is_ok();
    Json(json!({ "deleted": ok })).into_response()
}

async fn done() -> Response {
    StatusCode::NO_CONTENT.into_response()
}

fn announce(device_id: &str, port: u16) -> mdns_sd::ServiceDaemon {
    let daemon = mdns_sd::ServiceDaemon::new().expect("mDNS daemon");
    let _ = daemon.enable_interface(mdns_sd::IfKind::LoopbackV4);
    let info = mdns_sd::ServiceInfo::new(
        "_cprec._tcp.local.",
        &format!("cprec-{device_id}"),
        &format!("cprec-{device_id}.local."),
        "127.0.0.1",
        port,
        &[("id", device_id), ("v", "1")][..],
    )
    .expect("service info");
    daemon.register(info).expect("mDNS register");
    println!("announcing _cprec._tcp.local id={device_id}");
    daemon
}

#[tokio::main]
async fn main() {
    let args = Args::parse();
    std::fs::create_dir_all(&args.dir).ok();
    let addr = format!("127.0.0.1:{}", args.port);
    println!("mock-device on http://{addr}  dir={:?}  fault={:?}", args.dir, args.fault);
    // Kept alive for the life of the process.
    let _mdns = args.mdns.then(|| announce(&args.device_id, args.port));
    let app = App {
        args: Arc::new(args),
        inner: Arc::new(Mutex::new(Inner {
            acked: HashSet::new(),
            cursor: 0,
            signature: String::new(),
            clock_set: false,
        })),
    };
    let router = Router::new()
        .route("/v1/info", get(info))
        .route("/v1/time", post(time))
        .route("/v1/manifest", get(manifest))
        .route("/v1/recordings/:id", get(download).delete(delete_rec))
        .route("/v1/recordings/:id/sha256", get(sha))
        .route("/v1/recordings/:id/ack", post(ack))
        .route("/v1/done", post(done))
        .with_state(app);
    axum::serve(tokio::net::TcpListener::bind(&addr).await.unwrap(), router)
        .await
        .unwrap();
}
