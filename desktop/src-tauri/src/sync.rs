//! One sync pass against one device, following the architecture doc:
//! info, time, manifest since the stored cursor, then per recording:
//! download to `.part` (resuming), verify SHA-256 against the device, rename
//! into the library, write the SQLite row, and only then ack. The ack comes
//! last so a crash never leaves the device thinking a file is safe when it
//! is not.

use std::{
    path::{Path, PathBuf},
    sync::Mutex,
};

use serde::Serialize;
use sha2::{Digest, Sha256};

use crate::{
    db::{Db, RecordingRow},
    device::{Device, DeviceError},
    library,
    protocol::{ManifestEntry, is_valid_recording_id},
};

/// Attempts to finish one download when the connection keeps dropping.
const MAX_RESUMES: usize = 5;

#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase", rename_all_fields = "camelCase", tag = "phase")]
pub enum SyncEvent {
    Connected { device_id: String, device_name: String, firmware: String },
    Listed { count: usize },
    Downloading { index: usize, count: usize, name: String, done: u64, total: u64 },
    Verifying { index: usize, count: usize, name: String },
    Saved { index: usize, count: usize, name: String },
    FileFailed { name: String, message: String },
}

#[derive(Debug, Clone, Default, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct SyncReport {
    pub device_id: String,
    pub downloaded: usize,
    pub renamed: usize,
    pub reacked: usize,
    pub deleted_on_device: usize,
    pub failed: Vec<String>,
    pub cursor: u64,
}

#[derive(Debug, thiserror::Error)]
pub enum SyncError {
    #[error(transparent)]
    Device(#[from] DeviceError),
    #[error("database: {0}")]
    Db(#[from] rusqlite::Error),
    #[error("this address answered as device {found}, not {expected}")]
    WrongDevice { expected: String, found: String },
    #[error("the device sent an invalid recording id")]
    BadId,
}

impl SyncError {
    /// Short message for the UI, using the device's error codes.
    pub fn user_message(&self) -> String {
        match self {
            SyncError::Device(DeviceError::Api { code, .. }) if code == "busy" => {
                "The device is recording. Try again when it is idle.".into()
            }
            SyncError::Device(DeviceError::Api { code, .. }) if code == "low_battery" => {
                "The device battery is too low to sync. Charge it first.".into()
            }
            SyncError::Device(DeviceError::Unreachable(_)) => {
                "Could not reach the device. Press S on the Cardputer and check the address.".into()
            }
            other => other.to_string(),
        }
    }
}

pub struct SyncTarget<'a> {
    pub db: &'a Mutex<Db>,
    pub user_id: i64,
    pub library: PathBuf,
    /// Address to remember for this device (manual sync by IP).
    pub address: Option<String>,
    /// When syncing a known device, the id it must answer with.
    pub expected_device_id: Option<String>,
    pub now_ms: fn() -> i64,
    pub tz_offset_min: i32,
}

pub fn now_ms() -> i64 {
    chrono::Utc::now().timestamp_millis()
}

pub fn local_tz_offset_min() -> i32 {
    chrono::Local::now().offset().local_minus_utc() / 60
}

enum Outcome {
    Downloaded { deleted: bool },
    Renamed,
    Reacked,
    Unchanged,
}

pub async fn sync_device<D: Device>(
    device: &D,
    target: &SyncTarget<'_>,
    on_event: &(dyn Fn(SyncEvent) + Send + Sync),
) -> Result<SyncReport, SyncError> {
    let info = device.info().await?;
    if let Some(expected) = &target.expected_device_id {
        if *expected != info.device_id {
            return Err(SyncError::WrongDevice { expected: expected.clone(), found: info.device_id });
        }
    }
    let row = target.db.lock().unwrap().upsert_device_seen(
        target.user_id,
        &info,
        target.address.as_deref(),
        (target.now_ms)(),
    )?;
    on_event(SyncEvent::Connected {
        device_id: row.id.clone(),
        device_name: row.name.clone(),
        firmware: info.firmware.clone(),
    });

    device.set_time((target.now_ms)(), target.tz_offset_min).await?;
    let manifest = device.manifest(Some(row.last_cursor)).await?;
    let count = manifest.recordings.len();
    on_event(SyncEvent::Listed { count });

    let mut report = SyncReport { device_id: row.id.clone(), cursor: row.last_cursor, ..Default::default() };
    let mut all_ok = true;
    for (index, entry) in manifest.recordings.iter().enumerate() {
        match sync_one(device, target, &row.id, &row.name, entry, index, count, on_event).await {
            Ok(Outcome::Downloaded { deleted }) => {
                report.downloaded += 1;
                report.deleted_on_device += deleted as usize;
            }
            Ok(Outcome::Renamed) => report.renamed += 1,
            Ok(Outcome::Reacked) => report.reacked += 1,
            Ok(Outcome::Unchanged) => {}
            Err(SyncError::Device(e)) if !e.is_fatal() => {
                all_ok = false;
                report.failed.push(format!("{}: {e}", entry.name));
                on_event(SyncEvent::FileFailed { name: entry.name.clone(), message: e.to_string() });
            }
            Err(SyncError::BadId) => {
                all_ok = false;
                report.failed.push(format!("{}: invalid id", entry.name));
            }
            Err(e) => return Err(e),
        }
    }

    // Only move the cursor past this manifest when every entry is safe, so
    // failed files are listed again next time.
    if all_ok {
        target.db.lock().unwrap().set_device_cursor(target.user_id, &row.id, manifest.cursor)?;
        report.cursor = manifest.cursor;
    }
    if let Ok(after) = device.info().await {
        target.db.lock().unwrap().set_device_unsynced(target.user_id, &row.id, after.unsynced)?;
    }
    // Lets the device shut its server and WiFi down. Not fatal if it fails.
    let _ = device.done().await;
    Ok(report)
}

#[allow(clippy::too_many_arguments)]
async fn sync_one<D: Device>(
    device: &D,
    target: &SyncTarget<'_>,
    device_id: &str,
    device_name: &str,
    entry: &ManifestEntry,
    index: usize,
    count: usize,
    on_event: &(dyn Fn(SyncEvent) + Send + Sync),
) -> Result<Outcome, SyncError> {
    if !is_valid_recording_id(&entry.id) {
        return Err(SyncError::BadId);
    }

    let existing = target.db.lock().unwrap().get_recording(target.user_id, device_id, &entry.id)?;
    if let Some(existing) = existing {
        let mut outcome = Outcome::Unchanged;
        if existing.name != entry.name {
            // Same id, new name: update only the display name. The file on
            // disk keeps its original name.
            target
                .db
                .lock()
                .unwrap()
                .set_recording_device_name(target.user_id, device_id, &entry.id, &entry.name)?;
            outcome = Outcome::Renamed;
        }
        if !entry.acked {
            // Saved last time but the ack never arrived (crash or drop).
            let ack = device.ack(&entry.id, &existing.sha256).await?;
            target
                .db
                .lock()
                .unwrap()
                .set_recording_acked(target.user_id, device_id, &entry.id, !ack.deleted)?;
            outcome = Outcome::Reacked;
        }
        return Ok(outcome);
    }

    let part = library::partial_path(&target.library, &entry.id);
    tokio::fs::create_dir_all(part.parent().unwrap()).await.map_err(DeviceError::from)?;

    let mut verified: Option<String> = None;
    for attempt in 0..2 {
        download_with_resume(device, &entry.id, &part, entry, index, count, on_event).await?;
        on_event(SyncEvent::Verifying { index, count, name: entry.name.clone() });
        let local = hash_file(&part).await.map_err(DeviceError::from)?;
        let remote = device.sha256(&entry.id).await?;
        if local == remote {
            verified = Some(local);
            break;
        }
        let _ = tokio::fs::remove_file(&part).await;
        if attempt == 1 {
            return Err(DeviceError::Api {
                status: 409,
                code: "hash_mismatch".into(),
                message: "downloaded file does not match the device's SHA-256".into(),
            }
            .into());
        }
    }
    let sha256 = verified.expect("loop breaks only when verified");

    let synced_at = (target.now_ms)();
    let destination = unique_path(library::final_path(
        &target.library,
        device_name,
        &entry.name,
        &entry.id,
        entry.created_ms,
        synced_at,
    ));
    tokio::fs::create_dir_all(destination.parent().unwrap()).await.map_err(DeviceError::from)?;
    tokio::fs::rename(&part, &destination).await.map_err(DeviceError::from)?;

    let row = RecordingRow {
        id: entry.id.clone(),
        device_id: device_id.to_string(),
        device_name: device_name.to_string(),
        name: entry.name.clone(),
        display_name: None,
        size: entry.size,
        sha256: sha256.clone(),
        created_ms: entry.created_ms,
        duration_ms: entry.duration_ms,
        path: destination.to_string_lossy().into_owned(),
        synced_at_ms: synced_at,
        acked: false,
        on_device: true,
    };
    target.db.lock().unwrap().insert_recording(target.user_id, &row)?;
    on_event(SyncEvent::Saved { index, count, name: entry.name.clone() });

    let ack = device.ack(&entry.id, &sha256).await?;
    target
        .db
        .lock()
        .unwrap()
        .set_recording_acked(target.user_id, device_id, &entry.id, !ack.deleted)?;
    Ok(Outcome::Downloaded { deleted: ack.deleted })
}

async fn download_with_resume<D: Device>(
    device: &D,
    id: &str,
    part: &Path,
    entry: &ManifestEntry,
    index: usize,
    count: usize,
    on_event: &(dyn Fn(SyncEvent) + Send + Sync),
) -> Result<(), SyncError> {
    let total = entry.size;
    let name = entry.name.clone();
    let progress = move |done: u64| {
        on_event(SyncEvent::Downloading { index, count, name: name.clone(), done, total });
    };
    let mut last_error = None;
    for _ in 0..MAX_RESUMES {
        match device.download(id, part, &progress).await {
            Ok(len) if len == total => return Ok(()),
            Ok(len) if len > total => {
                // Stale or foreign part file: start over.
                let _ = tokio::fs::remove_file(part).await;
                last_error = Some(DeviceError::Protocol(format!("got {len} bytes, expected {total}")));
            }
            Ok(len) => {
                last_error = Some(DeviceError::Interrupted(format!("got {len} of {total} bytes")));
            }
            Err(e @ DeviceError::Interrupted(_)) => last_error = Some(e),
            Err(e) => return Err(e.into()),
        }
    }
    Err(last_error.unwrap_or_else(|| DeviceError::Interrupted("download failed".into())).into())
}

/// SHA-256 of a file as lowercase hex, computed off the async runtime.
pub async fn hash_file(path: &Path) -> std::io::Result<String> {
    let path = path.to_path_buf();
    tokio::task::spawn_blocking(move || {
        use std::io::Read;
        let mut file = std::fs::File::open(path)?;
        let mut hasher = Sha256::new();
        let mut buf = vec![0u8; 64 * 1024];
        loop {
            let n = file.read(&mut buf)?;
            if n == 0 {
                break;
            }
            hasher.update(&buf[..n]);
        }
        Ok(hasher.finalize().iter().map(|b| format!("{b:02x}")).collect())
    })
    .await
    .map_err(std::io::Error::other)?
}

/// Two recordings can map to the same file name only if the device reused
/// an id prefix; add a counter rather than overwrite.
fn unique_path(path: PathBuf) -> PathBuf {
    if !path.exists() {
        return path;
    }
    let stem = path.file_stem().unwrap_or_default().to_string_lossy().into_owned();
    for n in 1.. {
        let candidate = path.with_file_name(format!("{stem}-{n}.wav"));
        if !candidate.exists() {
            return candidate;
        }
    }
    unreachable!()
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::protocol::{AckResult, DeviceInfo, Manifest};
    use std::{
        collections::BTreeMap,
        sync::atomic::{AtomicUsize, Ordering},
    };

    /// In-memory device with the same rules as the firmware: cursor bumps on
    /// change, ack checks the hash, delete-after-sync spares locked files.
    #[derive(Default)]
    pub struct FakeDevice {
        pub state: Mutex<FakeState>,
        /// Next N downloads send half the bytes then fail.
        pub drop_next: AtomicUsize,
        /// Next N sha256 answers are wrong.
        pub bad_hash_next: AtomicUsize,
        pub busy: bool,
        pub range_starts: Mutex<Vec<u64>>,
        pub done_calls: AtomicUsize,
    }

    #[derive(Default)]
    pub struct FakeState {
        pub cursor: u64,
        pub recs: BTreeMap<String, FakeRec>,
        pub delete_after_sync: bool,
        pub clock_ms: Option<i64>,
    }

    #[derive(Clone)]
    pub struct FakeRec {
        pub name: String,
        pub data: Vec<u8>,
        pub locked: bool,
        pub acked: bool,
        pub changed: u64,
    }

    pub fn sha_hex(data: &[u8]) -> String {
        Sha256::digest(data).iter().map(|b| format!("{b:02x}")).collect()
    }

    impl FakeDevice {
        pub fn add(&self, id: &str, name: &str, data: Vec<u8>) {
            let mut s = self.state.lock().unwrap();
            s.cursor += 1;
            let changed = s.cursor;
            s.recs.insert(id.into(), FakeRec { name: name.into(), data, locked: false, acked: false, changed });
        }
        pub fn rename(&self, id: &str, name: &str) {
            let mut s = self.state.lock().unwrap();
            s.cursor += 1;
            let c = s.cursor;
            let r = s.recs.get_mut(id).unwrap();
            r.name = name.into();
            r.changed = c;
        }
        fn gate(&self) -> Result<(), DeviceError> {
            if self.busy {
                return Err(DeviceError::Api { status: 409, code: "busy".into(), message: "recording".into() });
            }
            Ok(())
        }
        fn not_found() -> DeviceError {
            DeviceError::Api { status: 404, code: "not_found".into(), message: "no such recording".into() }
        }
    }

    impl Device for FakeDevice {
        async fn info(&self) -> Result<DeviceInfo, DeviceError> {
            let s = self.state.lock().unwrap();
            Ok(DeviceInfo {
                device_id: "0123456789abcdef".into(),
                firmware: "fake".into(),
                battery_pct: Some(80),
                free_bytes: 1 << 30,
                unsynced: s.recs.values().filter(|r| !r.acked).count() as u64,
                clock_set: s.clock_ms.is_some(),
            })
        }
        async fn set_time(&self, unix_ms: i64, _tz: i32) -> Result<(), DeviceError> {
            self.gate()?;
            self.state.lock().unwrap().clock_ms = Some(unix_ms);
            Ok(())
        }
        async fn manifest(&self, since: Option<u64>) -> Result<Manifest, DeviceError> {
            self.gate()?;
            let s = self.state.lock().unwrap();
            let since = since.unwrap_or(0);
            Ok(Manifest {
                cursor: s.cursor,
                recordings: s
                    .recs
                    .iter()
                    .filter(|(_, r)| r.changed > since)
                    .map(|(id, r)| ManifestEntry {
                        id: id.clone(),
                        name: r.name.clone(),
                        size: r.data.len() as u64,
                        created_ms: 0,
                        duration_ms: 0,
                        locked: r.locked,
                        acked: r.acked,
                    })
                    .collect(),
            })
        }
        async fn download(
            &self,
            id: &str,
            part: &Path,
            progress: &(dyn Fn(u64) + Send + Sync),
        ) -> Result<u64, DeviceError> {
            let data = self.state.lock().unwrap().recs.get(id).ok_or_else(Self::not_found)?.data.clone();
            let offset = std::fs::metadata(part).map(|m| m.len()).unwrap_or(0);
            self.range_starts.lock().unwrap().push(offset);
            let rest = &data[offset as usize..];
            let drop = self
                .drop_next
                .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| n.checked_sub(1))
                .is_ok();
            let send = if drop { &rest[..rest.len() / 2] } else { rest };
            use std::io::Write;
            let mut f = std::fs::OpenOptions::new().create(true).append(true).open(part)?;
            f.write_all(send)?;
            let len = offset + send.len() as u64;
            progress(len);
            if drop {
                return Err(DeviceError::Interrupted("fake drop".into()));
            }
            Ok(len)
        }
        async fn sha256(&self, id: &str) -> Result<String, DeviceError> {
            let data = self.state.lock().unwrap().recs.get(id).ok_or_else(Self::not_found)?.data.clone();
            let bad = self
                .bad_hash_next
                .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| n.checked_sub(1))
                .is_ok();
            Ok(if bad { "0".repeat(64) } else { sha_hex(&data) })
        }
        async fn ack(&self, id: &str, sha256: &str) -> Result<AckResult, DeviceError> {
            let mut s = self.state.lock().unwrap();
            let delete_after = s.delete_after_sync;
            s.cursor += 1;
            let c = s.cursor;
            let r = s.recs.get_mut(id).ok_or_else(Self::not_found)?;
            if sha_hex(&r.data) != sha256 {
                return Err(DeviceError::Api { status: 409, code: "hash_mismatch".into(), message: String::new() });
            }
            r.acked = true;
            r.changed = c;
            let deleted = delete_after && !r.locked;
            if deleted {
                s.recs.remove(id);
            }
            Ok(AckResult { acked: true, deleted })
        }
        async fn delete(&self, id: &str) -> Result<bool, DeviceError> {
            Ok(self.state.lock().unwrap().recs.remove(id).is_some())
        }
        async fn done(&self) -> Result<(), DeviceError> {
            self.done_calls.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
    }

    const ID1: &str = "11111111111111111111111111111111";
    const ID2: &str = "22222222222222222222222222222222";

    fn fixed_now() -> i64 {
        1_763_290_000_000
    }

    struct Fixture {
        _dir: tempfile::TempDir,
        library: PathBuf,
        db: Mutex<Db>,
        user_id: i64,
    }

    impl Fixture {
        fn new() -> Self {
            let dir = tempfile::tempdir().unwrap();
            let library = dir.path().join("lib");
            let db = Db::open_in_memory().unwrap();
            let user_id = db.create_user("u", "h", &library.to_string_lossy(), 0).unwrap().id;
            Self { _dir: dir, library, db: Mutex::new(db), user_id }
        }
        fn target(&self) -> SyncTarget<'_> {
            SyncTarget {
                db: &self.db,
                user_id: self.user_id,
                library: self.library.clone(),
                address: Some("http://127.0.0.1:8080".into()),
                expected_device_id: None,
                now_ms: fixed_now,
                tz_offset_min: 0,
            }
        }
        fn rows(&self) -> Vec<RecordingRow> {
            self.db.lock().unwrap().list_recordings(self.user_id).unwrap()
        }
    }

    fn audio(n: usize, seed: u8) -> Vec<u8> {
        (0..n).map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed)).collect()
    }

    async fn run(dev: &FakeDevice, fx: &Fixture) -> Result<SyncReport, SyncError> {
        sync_device(dev, &fx.target(), &|_| {}).await
    }

    #[tokio::test]
    async fn first_sync_downloads_verifies_saves_and_acks() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        dev.add(ID1, "REC0001.WAV", audio(70_000, 1));
        dev.add(ID2, "REC0002.WAV", audio(1_000, 2));

        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.downloaded, 2);
        assert!(report.failed.is_empty());

        let rows = fx.rows();
        assert_eq!(rows.len(), 2);
        for row in &rows {
            assert!(row.acked);
            let bytes = std::fs::read(&row.path).unwrap();
            assert_eq!(sha_hex(&bytes), row.sha256);
            assert!(row.path.contains("Cardputer 0123"));
        }
        let s = dev.state.lock().unwrap();
        assert!(s.recs.values().all(|r| r.acked));
        assert_eq!(s.clock_ms, Some(fixed_now()));
        drop(s);
        assert_eq!(dev.done_calls.load(Ordering::SeqCst), 1);
        assert!(!fx.library.join(".partial").join(format!("{ID1}.wav.part")).exists());
    }

    #[tokio::test]
    async fn second_sync_only_sees_changes() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        dev.add(ID1, "REC0001.WAV", audio(5_000, 1));
        run(&dev, &fx).await.unwrap();

        dev.add(ID2, "REC0002.WAV", audio(5_000, 2));
        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.downloaded, 1);
        assert_eq!(fx.rows().len(), 2);
    }

    #[tokio::test]
    async fn interrupted_download_resumes_from_part_file() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        let data = audio(64_000, 3);
        dev.add(ID1, "REC0001.WAV", data.clone());
        dev.drop_next.store(2, Ordering::SeqCst);

        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.downloaded, 1);
        // 0, then half, then three quarters: each attempt resumed.
        assert_eq!(*dev.range_starts.lock().unwrap(), vec![0, 32_000, 48_000]);
        assert_eq!(std::fs::read(&fx.rows()[0].path).unwrap(), data);
    }

    #[tokio::test]
    async fn hash_mismatch_retries_once_then_fails_without_ack() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        dev.add(ID1, "REC0001.WAV", audio(2_000, 4));

        dev.bad_hash_next.store(1, Ordering::SeqCst);
        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.downloaded, 1, "one bad hash is retried");

        let fx2 = Fixture::new();
        let dev2 = FakeDevice::default();
        dev2.add(ID1, "REC0001.WAV", audio(2_000, 4));
        dev2.bad_hash_next.store(2, Ordering::SeqCst);
        let report = run(&dev2, &fx2).await.unwrap();
        assert_eq!(report.downloaded, 0);
        assert_eq!(report.failed.len(), 1);
        assert!(fx2.rows().is_empty());
        assert!(!dev2.state.lock().unwrap().recs[ID1].acked);
        // Cursor not advanced, so the next sync lists the file again.
        assert_eq!(report.cursor, 0);
        let again = run(&dev2, &fx2).await.unwrap();
        assert_eq!(again.downloaded, 1);
    }

    #[tokio::test]
    async fn rename_on_device_updates_name_only() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        dev.add(ID1, "REC0001.WAV", audio(1_000, 5));
        run(&dev, &fx).await.unwrap();
        let path_before = fx.rows()[0].path.clone();

        dev.rename(ID1, "MEETING.WAV");
        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.renamed, 1);
        assert_eq!(report.downloaded, 0);
        let row = &fx.rows()[0];
        assert_eq!(row.name, "MEETING.WAV");
        assert_eq!(row.path, path_before);
    }

    #[tokio::test]
    async fn missing_ack_is_resent_from_the_library() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        let data = audio(1_000, 6);
        dev.add(ID1, "REC0001.WAV", data.clone());
        // Simulate a crash after the SQLite row was written but before ack.
        fx.db
            .lock()
            .unwrap()
            .insert_recording(
                fx.user_id,
                &RecordingRow {
                    id: ID1.into(),
                    device_id: "0123456789abcdef".into(),
                    device_name: "x".into(),
                    name: "REC0001.WAV".into(),
                    display_name: None,
                    size: 1_000,
                    sha256: sha_hex(&data),
                    created_ms: 0,
                    duration_ms: 0,
                    path: "x".into(),
                    synced_at_ms: 0,
                    acked: false,
                    on_device: true,
                },
            )
            .unwrap();
        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.reacked, 1);
        assert_eq!(report.downloaded, 0);
        assert!(dev.state.lock().unwrap().recs[ID1].acked);
        assert!(fx.rows()[0].acked);
    }

    #[tokio::test]
    async fn delete_after_sync_spares_locked_files() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        dev.state.lock().unwrap().delete_after_sync = true;
        dev.add(ID1, "REC0001.WAV", audio(1_000, 7));
        dev.add(ID2, "KEEP.WAV", audio(1_000, 8));
        dev.state.lock().unwrap().recs.get_mut(ID2).unwrap().locked = true;

        let report = run(&dev, &fx).await.unwrap();
        assert_eq!(report.deleted_on_device, 1);
        let s = dev.state.lock().unwrap();
        assert!(!s.recs.contains_key(ID1));
        assert!(s.recs.contains_key(ID2));
    }

    #[test]
    fn events_serialize_for_the_ui() {
        let json = serde_json::to_value(SyncEvent::Downloading {
            index: 0,
            count: 2,
            name: "A.WAV".into(),
            done: 1,
            total: 2,
        })
        .unwrap();
        assert_eq!(json["phase"], "downloading");
        assert_eq!(json["total"], 2);
        let json = serde_json::to_value(SyncEvent::Connected {
            device_id: "d".into(),
            device_name: "n".into(),
            firmware: "f".into(),
        })
        .unwrap();
        assert_eq!(json["deviceId"], "d");
        assert_eq!(json["deviceName"], "n");
    }

    #[tokio::test]
    async fn busy_device_stops_the_sync() {
        let fx = Fixture::new();
        let dev = FakeDevice { busy: true, ..Default::default() };
        let err = run(&dev, &fx).await.unwrap_err();
        assert!(err.user_message().contains("recording"));
    }

    #[tokio::test]
    async fn wrong_device_at_address_is_rejected() {
        let fx = Fixture::new();
        let dev = FakeDevice::default();
        let mut target = fx.target();
        target.expected_device_id = Some("ffffffffffffffff".into());
        let err = sync_device(&dev, &target, &|_| {}).await.unwrap_err();
        assert!(matches!(err, SyncError::WrongDevice { .. }));
    }
}
