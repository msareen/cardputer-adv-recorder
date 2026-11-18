//! Cardputer Sync desktop app. Rust owns all device work (BLE, HTTPS, files,
//! SQLite, accounts, keys); React only renders and calls the commands below.

pub mod auth;
pub mod ble;
pub mod ble_sync;
pub mod db;
pub mod device;
pub mod discovery;
pub mod identity;
pub mod library;
pub mod pairing;
pub mod protocol;
pub mod sync;
pub mod tls;

use std::{
    collections::{HashMap, HashSet},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use serde::Serialize;
use tauri::{
    AppHandle, Emitter, Manager, State, WindowEvent,
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
};
use tauri_plugin_autostart::{MacosLauncher, ManagerExt};

use crate::{
    ble::{BleLink, NearbyDevice},
    db::{Db, DeviceRow, RecordingRow, User},
    ble_sync::BleDevice,
    device::{Device, HttpDevice, normalize_address, normalize_paired_address},
    discovery::{DiscoveryEvent, Presence},
    identity::HostIdentity,
    pairing::{PublicKeyBytes, Session, WifiResult},
    sync::{SyncEvent, SyncReport, SyncTarget},
};

/// A pairing in progress: the BLE session plus what it is for.
struct Pairing {
    session: Session<BleLink>,
    peripheral: btleplug::platform::Peripheral,
}

pub struct AppState {
    db: Mutex<Db>,
    session: Mutex<Option<User>>,
    /// The logged-in user's host key, loaded from the keychain on first use.
    host: Mutex<Option<Arc<HostIdentity>>>,
    /// Device ids with a sync in flight, so one device never syncs twice
    /// at once.
    syncing: Mutex<HashSet<String>>,
    /// Peripherals from the last BLE scan, by device id.
    nearby: Mutex<HashMap<String, btleplug::platform::Peripheral>>,
    pairing: tokio::sync::Mutex<Option<Pairing>>,
    /// Devices announcing on the LAN (milestone 5 discovery).
    presence: Mutex<Presence>,
    /// Keeps mDNS browsing alive.
    mdns: Mutex<Option<mdns_sd::ServiceDaemon>>,
}

const REMEMBERED_USER: &str = "remembered_user";
const MAIN_WINDOW: &str = "main";
const TRAY_ID: &str = "main";

type CmdResult<T> = Result<T, String>;

fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

fn current_user(state: &AppState) -> CmdResult<User> {
    state.session.lock().unwrap().clone().ok_or_else(|| "Not logged in.".to_string())
}

fn host_identity(state: &AppState, user: &User) -> CmdResult<Arc<HostIdentity>> {
    let mut cached = state.host.lock().unwrap();
    if let Some(host) = cached.as_ref() {
        return Ok(host.clone());
    }
    let host = Arc::new(HostIdentity::load_or_create(user.id, &user.username)?);
    *cached = Some(host.clone());
    Ok(host)
}

/// HTTPS pinned to the device key for paired devices; plain HTTP for dev
/// devices added by address (tools/mock-device).
fn device_client(state: &AppState, user: &User, row: &DeviceRow) -> CmdResult<HttpDevice> {
    let address = row.address.as_deref().ok_or("No address for this device yet. Edit it, or pair again.")?;
    match state.db.lock().unwrap().device_public_key(user.id, &row.id).map_err(err)? {
        Some(key) => {
            let key: PublicKeyBytes = key.try_into().map_err(|_| "Stored device key is damaged.".to_string())?;
            let host = host_identity(state, user)?;
            HttpDevice::paired(address, tls::client_config(&key, &host)?)
        }
        None => HttpDevice::new(address),
    }
}

fn default_library(app: &AppHandle, username: &str) -> PathBuf {
    let base = app
        .path()
        .audio_dir()
        .or_else(|_| app.path().document_dir())
        .or_else(|_| app.path().app_data_dir())
        .unwrap_or_else(|_| PathBuf::from("."));
    base.join("Cardputer Sync").join(library::sanitize_component(username))
}

/// Lets the webview play files from the user's library through the asset
/// protocol, and nothing outside it.
fn allow_library(app: &AppHandle, library: &Path) {
    let _ = std::fs::create_dir_all(library);
    if let Err(e) = app.asset_protocol_scope().allow_directory(library, true) {
        eprintln!("asset scope: {e}");
    }
}

fn start_session(app: &AppHandle, state: &AppState, user: &User) {
    allow_library(app, Path::new(&user.library_path));
    *state.host.lock().unwrap() = None;
    *state.session.lock().unwrap() = Some(user.clone());
}

#[tauri::command]
fn session(state: State<'_, AppState>) -> Option<User> {
    state.session.lock().unwrap().clone()
}

#[tauri::command]
fn create_account(app: AppHandle, state: State<'_, AppState>, username: String, password: String) -> CmdResult<User> {
    auth::validate_new_account(&username, &password)?;
    let username = username.trim().to_string();
    let hash = auth::hash_password(&password)?;
    let library = default_library(&app, &username);
    let user = state
        .db
        .lock()
        .unwrap()
        .create_user(&username, &hash, &library.to_string_lossy(), sync::now_ms())
        .map_err(|e| match e {
            rusqlite::Error::SqliteFailure(f, _) if f.code == rusqlite::ErrorCode::ConstraintViolation => {
                "That username is taken.".to_string()
            }
            other => other.to_string(),
        })?;
    start_session(&app, &state, &user);
    Ok(user)
}

#[tauri::command]
fn login(app: AppHandle, state: State<'_, AppState>, username: String, password: String) -> CmdResult<User> {
    let found = state.db.lock().unwrap().find_user(username.trim()).map_err(err)?;
    let user = match found {
        Some((user, hash)) if auth::verify_password(&password, &hash) => user,
        _ => return Err("Wrong username or password.".into()),
    };
    start_session(&app, &state, &user);
    Ok(user)
}

#[tauri::command]
async fn logout(state: State<'_, AppState>) -> CmdResult<()> {
    if let Some(p) = state.pairing.lock().await.take() {
        p.session.into_channel().disconnect().await;
    }
    *state.session.lock().unwrap() = None;
    *state.host.lock().unwrap() = None;
    state.db.lock().unwrap().set_setting(REMEMBERED_USER, None).map_err(err)?;
    Ok(())
}

// --- app settings ---------------------------------------------------------------

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AppSettings {
    auto_sync: bool,
    stay_logged_in: bool,
    start_at_login: bool,
}

#[tauri::command]
fn app_settings(app: AppHandle, state: State<'_, AppState>) -> CmdResult<AppSettings> {
    let user = current_user(&state)?;
    let remembered = state.db.lock().unwrap().setting(REMEMBERED_USER).map_err(err)?;
    Ok(AppSettings {
        auto_sync: user.auto_sync,
        stay_logged_in: remembered.as_deref() == Some(user.id.to_string().as_str()),
        start_at_login: app.autolaunch().is_enabled().unwrap_or(false),
    })
}

#[tauri::command]
fn set_auto_sync(state: State<'_, AppState>, enabled: bool) -> CmdResult<()> {
    let user = current_user(&state)?;
    state.db.lock().unwrap().set_auto_sync(user.id, enabled).map_err(err)?;
    if let Some(session) = state.session.lock().unwrap().as_mut() {
        session.auto_sync = enabled;
    }
    Ok(())
}

/// Single-user installs: open straight into this account next time.
#[tauri::command]
fn set_stay_logged_in(state: State<'_, AppState>, enabled: bool) -> CmdResult<()> {
    let user = current_user(&state)?;
    let value = enabled.then(|| user.id.to_string());
    state.db.lock().unwrap().set_setting(REMEMBERED_USER, value.as_deref()).map_err(err)
}

/// Starts hidden in the tray at login, so auto-sync works without opening
/// the window.
#[tauri::command]
fn set_start_at_login(app: AppHandle, enabled: bool) -> CmdResult<()> {
    let launcher = app.autolaunch();
    if enabled { launcher.enable() } else { launcher.disable() }.map_err(err)
}

/// Device ids currently announcing sync mode on the LAN.
#[tauri::command]
fn present_devices(state: State<'_, AppState>) -> Vec<String> {
    state.presence.lock().unwrap().present_ids()
}

#[tauri::command]
fn set_library_path(app: AppHandle, state: State<'_, AppState>, path: String) -> CmdResult<User> {
    let user = current_user(&state)?;
    let trimmed = path.trim();
    if trimmed.is_empty() || !Path::new(trimmed).is_absolute() {
        return Err("Enter an absolute folder path.".into());
    }
    std::fs::create_dir_all(trimmed).map_err(|e| format!("Cannot use that folder: {e}"))?;
    state.db.lock().unwrap().set_library_path(user.id, trimmed).map_err(err)?;
    allow_library(&app, Path::new(trimmed));
    let updated = User { library_path: trimmed.to_string(), ..user };
    *state.session.lock().unwrap() = Some(updated.clone());
    Ok(updated)
}

#[tauri::command]
fn list_devices(state: State<'_, AppState>) -> CmdResult<Vec<DeviceRow>> {
    let user = current_user(&state)?;
    state.db.lock().unwrap().list_devices(user.id).map_err(err)
}

/// Dev only: adds an unpaired device by address and talks plain HTTP to it
/// (tools/mock-device). Real devices are added by pairing over Bluetooth.
#[tauri::command]
async fn add_device(state: State<'_, AppState>, address: String) -> CmdResult<DeviceRow> {
    let user = current_user(&state)?;
    let device = HttpDevice::new(&address)?;
    let info = device
        .info()
        .await
        .map_err(|e| format!("No dev device answered at {}. ({e})", device.base()))?;
    state
        .db
        .lock()
        .unwrap()
        .upsert_device_seen(user.id, &info, Some(device.base()), sync::now_ms())
        .map_err(err)
}

#[tauri::command]
fn rename_device(state: State<'_, AppState>, device_id: String, name: String) -> CmdResult<()> {
    let user = current_user(&state)?;
    let name = name.trim();
    if name.is_empty() {
        return Err("Name required.".into());
    }
    state.db.lock().unwrap().rename_device(user.id, &device_id, name).map_err(err)
}

#[tauri::command]
fn set_device_address(state: State<'_, AppState>, device_id: String, address: String) -> CmdResult<()> {
    let user = current_user(&state)?;
    let db = state.db.lock().unwrap();
    let paired = db.device_public_key(user.id, &device_id).map_err(err)?.is_some();
    let base = if paired { normalize_paired_address(&address)? } else { normalize_address(&address)? };
    db.set_device_address(user.id, &device_id, &base).map_err(err)
}

/// Forgets the device on this desktop only. Synced files stay. The device
/// keeps its pairing until "Forget pairing" is chosen on it.
#[tauri::command]
fn remove_device(state: State<'_, AppState>, device_id: String) -> CmdResult<()> {
    let user = current_user(&state)?;
    state.db.lock().unwrap().remove_device(user.id, &device_id).map_err(err)
}

// --- pairing over Bluetooth ---------------------------------------------------

#[tauri::command]
async fn ble_scan(state: State<'_, AppState>) -> CmdResult<Vec<NearbyDevice>> {
    current_user(&state)?;
    let (found, peripherals) = ble::scan(Duration::from_secs(4)).await.map_err(err)?;
    *state.nearby.lock().unwrap() = peripherals;
    Ok(found)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PairStart {
    device_id: String,
    /// Six digits to compare with the Cardputer; None when the device
    /// already knows this desktop (changing WiFi).
    code: Option<String>,
    known: bool,
}

#[tauri::command]
async fn pair_start(state: State<'_, AppState>, device_id: String) -> CmdResult<PairStart> {
    let user = current_user(&state)?;
    let host = host_identity(&state, &user)?;
    let peripheral = state
        .nearby
        .lock()
        .unwrap()
        .get(&device_id)
        .cloned()
        .ok_or("That device is no longer in range. Scan again.")?;

    let mut slot = state.pairing.lock().await;
    if let Some(old) = slot.take() {
        old.session.into_channel().disconnect().await;
    }
    let link = BleLink::connect(peripheral.clone()).await.map_err(err)?;
    let session = match Session::open(link, &host.secret).await {
        Ok(session) => session,
        Err(e) => {
            use btleplug::api::Peripheral as _;
            let _ = peripheral.disconnect().await;
            return Err(e.to_string());
        }
    };
    if session.device_id != device_id {
        session.into_channel().disconnect().await;
        return Err("The device's key does not match what it advertised.".into());
    }
    let start = PairStart { device_id: session.device_id.clone(), code: session.code.clone(), known: session.known };
    *slot = Some(Pairing { session, peripheral });
    Ok(start)
}

/// After the user confirmed the code here: waits for Enter on the device
/// (skipped for a known device), then returns the networks it can see.
#[tauri::command]
async fn pair_ready(state: State<'_, AppState>) -> CmdResult<Vec<String>> {
    let mut slot = state.pairing.lock().await;
    let pairing = slot.as_mut().ok_or("No pairing in progress.")?;
    if !pairing.session.known {
        pairing.session.wait_confirmed(Duration::from_secs(120)).await.map_err(err)?;
    }
    pairing.session.networks().await.map_err(err)
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PairOutcome {
    device: DeviceRow,
    wifi: WifiResult,
}

#[tauri::command]
async fn pair_finish(
    state: State<'_, AppState>,
    ssid: String,
    password: String,
    bluetooth_only: Option<bool>,
) -> CmdResult<PairOutcome> {
    let user = current_user(&state)?;
    let host = host_identity(&state, &user)?;
    // Bluetooth only: the device gets an empty network, so it stores no
    // WiFi and its S key goes straight to the BLE sync.
    let bluetooth_only = bluetooth_only.unwrap_or(false);
    let (ssid, password) = if bluetooth_only { (String::new(), String::new()) } else { (ssid, password) };
    if !bluetooth_only && (ssid.trim().is_empty() || ssid.len() > 32) {
        return Err("Choose a WiFi network (up to 32 bytes).".into());
    }
    if password.len() > 63 {
        return Err("WiFi passwords are at most 63 characters.".into());
    }
    let mut slot = state.pairing.lock().await;
    let mut pairing = slot.take().ok_or("No pairing in progress.")?;
    let result = async {
        if pairing.session.known {
            pairing.session.send_set_wifi(&ssid, &password).await?;
        } else {
            pairing.session.send_pair(&user.username, &host.cert_der, &ssid, &password).await?;
        }
        pairing.session.wifi_result().await
    }
    .await;
    let device_id = pairing.session.device_id.clone();
    let device_key = pairing.session.device_pk;
    pairing.session.into_channel().disconnect().await;
    let _ = pairing.peripheral;
    let wifi = result.map_err(err)?;

    let address = if bluetooth_only { None } else { wifi.ip.as_deref().map(normalize_paired_address).transpose()? };
    let device = state
        .db
        .lock()
        .unwrap()
        .save_paired_device(user.id, &device_id, &device_key, address.as_deref(), None, bluetooth_only, sync::now_ms())
        .map_err(err)?;
    Ok(PairOutcome { device, wifi })
}

#[tauri::command]
async fn pair_cancel(state: State<'_, AppState>) -> CmdResult<()> {
    if let Some(p) = state.pairing.lock().await.take() {
        p.session.into_channel().disconnect().await;
    }
    Ok(())
}

// --- sync ---------------------------------------------------------------------

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct ProgressPayload {
    device_id: String,
    event: SyncEvent,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct SyncStarted {
    device_id: String,
    automatic: bool,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct SyncFinished {
    device_id: String,
    automatic: bool,
    report: Option<SyncReport>,
    error: Option<String>,
}

#[tauri::command]
async fn sync_device(app: AppHandle, device_id: String) -> CmdResult<SyncReport> {
    let state = app.state::<AppState>();
    let user = current_user(&state)?;
    let ble_only = state.db.lock().unwrap().get_device(user.id, &device_id).map_err(err)?.is_some_and(|d| d.ble_only);
    if ble_only {
        // No WiFi on the device: Sync now means the Bluetooth fallback.
        return sync_device_ble(app, device_id).await;
    }
    run_sync(&app, &device_id, false).await
}

/// One sync, from the Sync now button or the discovery worker. Announces
/// itself with sync-started / sync-progress / sync-finished events so the
/// UI shows background syncs too.
async fn run_sync(app: &AppHandle, device_id: &str, automatic: bool) -> CmdResult<SyncReport> {
    let state = app.state::<AppState>();
    let user = current_user(&state)?;
    let row = state
        .db
        .lock()
        .unwrap()
        .get_device(user.id, device_id)
        .map_err(err)?
        .ok_or("Unknown device.")?;
    if row.ble_only {
        return Err("This device is set to Bluetooth only; it never joins WiFi.".into());
    }
    let device = device_client(&state, &user, &row)?;
    let address = Some(device.base().to_string());
    let (result, _) = run_sync_with(app, &user, &row, automatic, async { Ok((device, address)) }).await;
    result
}

/// Runs one sync over whatever `connect` yields (HTTPS, or the BLE
/// fallback) and returns the device so the caller can close it.
async fn run_sync_with<D: Device>(
    app: &AppHandle,
    user: &User,
    row: &DeviceRow,
    automatic: bool,
    connect: impl std::future::Future<Output = CmdResult<(D, Option<String>)>>,
) -> (CmdResult<SyncReport>, Option<D>) {
    let state = app.state::<AppState>();
    let device_id = row.id.clone();
    if !state.syncing.lock().unwrap().insert(device_id.clone()) {
        return (Err("This device is already syncing.".into()), None);
    }
    let _ = app.emit("sync-started", SyncStarted { device_id: device_id.clone(), automatic });
    let (device, address) = match connect.await {
        Ok(connected) => connected,
        Err(e) => {
            state.syncing.lock().unwrap().remove(&device_id);
            let _ = app.emit(
                "sync-finished",
                SyncFinished { device_id, automatic, report: None, error: Some(e.clone()) },
            );
            return (Err(e), None);
        }
    };
    let target = SyncTarget {
        db: &state.db,
        user_id: user.id,
        library: PathBuf::from(&user.library_path),
        address,
        expected_device_id: Some(device_id.clone()),
        now_ms: sync::now_ms,
        tz_offset_min: sync::local_tz_offset_min(),
    };
    let emit_id = device_id.clone();
    let emitter = app.clone();
    let on_event = move |event: SyncEvent| {
        let _ = emitter.emit("sync-progress", ProgressPayload { device_id: emit_id.clone(), event });
    };
    let result = sync::sync_device(&device, &target, &on_event).await;
    state.syncing.lock().unwrap().remove(&device_id);
    let result = result.map_err(|e| e.user_message());
    if let Ok(report) = &result {
        if report.downloaded > 0 {
            set_tray_status(app, &format!("Synced {} new from {}", report.downloaded, row.name));
        }
    }
    let _ = app.emit(
        "sync-finished",
        SyncFinished {
            device_id,
            automatic,
            report: result.as_ref().ok().cloned(),
            error: result.as_ref().err().cloned(),
        },
    );
    (result, Some(device))
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct BleEstimate {
    device_id: String,
    bytes: u64,
    secs: u64,
}

/// BLE fallback (milestone 6): finds the paired device advertising sync
/// mode, opens the known-host session and runs the normal sync over it.
#[tauri::command]
async fn sync_device_ble(app: AppHandle, device_id: String) -> CmdResult<SyncReport> {
    let state = app.state::<AppState>();
    let user = current_user(&state)?;
    let row = state
        .db
        .lock()
        .unwrap()
        .get_device(user.id, &device_id)
        .map_err(err)?
        .ok_or("Unknown device.")?;
    let key: PublicKeyBytes = state
        .db
        .lock()
        .unwrap()
        .device_public_key(user.id, &device_id)
        .map_err(err)?
        .ok_or("Only paired devices can sync over Bluetooth.")?
        .try_into()
        .map_err(|_| "Stored device key is damaged.".to_string())?;
    let host = host_identity(&state, &user)?;

    let connect = async {
        let (found, mut peripherals) = ble::scan(Duration::from_secs(5)).await.map_err(err)?;
        let advertising = found.iter().any(|d| d.device_id == device_id && d.sync_mode);
        let peripheral = peripherals.remove(&device_id).filter(|_| advertising).ok_or(
            "The Cardputer is not waiting for Bluetooth sync. On it, choose Settings > Sync > \
             Sync over Bluetooth (or press Enter when WiFi fails), then try again.",
        )?;
        let link = BleLink::connect(peripheral.clone()).await.map_err(err)?;
        let session = match Session::open(link, &host.secret).await {
            Ok(s) if s.known && s.device_pk == key => s,
            Ok(s) => {
                s.into_channel().disconnect().await;
                return Err("This Cardputer is paired with another desktop.".to_string());
            }
            Err(e) => {
                use btleplug::api::Peripheral as _;
                let _ = peripheral.disconnect().await;
                return Err(e.to_string());
            }
        };
        let device = BleDevice::new(session);
        // The architecture doc asks for an estimate before the transfer.
        if let Ok(manifest) = device.manifest(None).await {
            let bytes: u64 = manifest.recordings.iter().filter(|r| !r.acked).map(|r| r.size).sum();
            let _ = app.emit(
                "ble-sync-estimate",
                BleEstimate { device_id: device_id.clone(), bytes, secs: ble_sync::estimate_secs(bytes) },
            );
        }
        Ok((device, None))
    };
    let (result, device) = run_sync_with(&app, &user, &row, false, connect).await;
    if let Some(device) = device {
        device.into_session().into_channel().disconnect().await;
    }
    result
}

// --- hands-free sync: discovery worker and tray ----------------------------------

fn set_tray_status(app: &AppHandle, status: &str) {
    if let Some(tray) = app.tray_by_id(TRAY_ID) {
        let _ = tray.set_tooltip(Some(format!("Cardputer Sync: {status}")));
    }
}

fn emit_presence(app: &AppHandle) {
    let ids = app.state::<AppState>().presence.lock().unwrap().present_ids();
    let _ = app.emit("presence-changed", ids);
}

/// Remembers where a discovered device is, if it belongs to the logged-in
/// user, so Sync now and delete on device use the current address.
fn record_address(app: &AppHandle, device_id: &str, address: &str) {
    let state = app.state::<AppState>();
    let Some(user) = state.session.lock().unwrap().clone() else { return };
    let db = state.db.lock().unwrap();
    let Ok(Some(_)) = db.get_device(user.id, device_id) else { return };
    let paired = matches!(db.device_public_key(user.id, device_id), Ok(Some(_)));
    let base = if paired { normalize_paired_address(address) } else { normalize_address(address) };
    if let Ok(base) = base {
        let _ = db.set_device_address(user.id, device_id, &base);
    }
}

/// Starts syncs for announcing devices that are due, if the user has
/// auto-sync on and owns them.
fn start_due_syncs(app: &AppHandle) {
    let state = app.state::<AppState>();
    let Some(user) = state.session.lock().unwrap().clone() else { return };
    if !user.auto_sync {
        return;
    }
    let now = Instant::now();
    let due = state.presence.lock().unwrap().due(now);
    for device_id in due {
        let owned = matches!(state.db.lock().unwrap().get_device(user.id, &device_id), Ok(Some(_)));
        if !owned || state.syncing.lock().unwrap().contains(&device_id) {
            continue;
        }
        if let Some(address) = state.presence.lock().unwrap().address(&device_id) {
            record_address(app, &device_id, address);
        }
        // Mark the attempt now so the next tick does not start it again.
        state.presence.lock().unwrap().record(&device_id, false, now);
        let app = app.clone();
        tauri::async_runtime::spawn(async move {
            let ok = run_sync(&app, &device_id, true).await.is_ok();
            app.state::<AppState>().presence.lock().unwrap().record(&device_id, ok, Instant::now());
        });
    }
}

async fn discovery_worker(app: AppHandle, mut events: tokio::sync::mpsc::UnboundedReceiver<DiscoveryEvent>) {
    let mut tick = tokio::time::interval(Duration::from_secs(3));
    loop {
        tokio::select! {
            event = events.recv() => {
                let Some(event) = event else { break };
                let state = app.state::<AppState>();
                match event {
                    DiscoveryEvent::Found(found) => {
                        let (id, address) = (found.device_id.clone(), found.address.clone());
                        if state.presence.lock().unwrap().found(found) {
                            record_address(&app, &id, &address);
                            emit_presence(&app);
                        }
                    }
                    DiscoveryEvent::Removed { fullname } => {
                        if state.presence.lock().unwrap().removed(&fullname).is_some() {
                            emit_presence(&app);
                        }
                    }
                }
            }
            _ = tick.tick() => start_due_syncs(&app),
        }
    }
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window(MAIN_WINDOW) {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
    }
}

fn build_tray(app: &tauri::App) -> tauri::Result<()> {
    let open = MenuItem::with_id(app, "open", "Open Cardputer Sync", true, None::<&str>)?;
    let sync = MenuItem::with_id(app, "sync", "Sync devices on the network", true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
    let menu = Menu::with_items(app, &[&open, &sync, &quit])?;
    let mut builder = TrayIconBuilder::with_id(TRAY_ID)
        .tooltip("Cardputer Sync")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_menu_event(|app, event| match event.id.as_ref() {
            "open" => show_main_window(app),
            "sync" => {
                app.state::<AppState>().presence.lock().unwrap().retry_all();
                start_due_syncs(app);
            }
            "quit" => app.exit(0),
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = event {
                show_main_window(tray.app_handle());
            }
        });
    if let Some(icon) = app.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    builder.build(app)?;
    Ok(())
}

/// Closing the window keeps the app in the tray while someone is logged in
/// with auto-sync on; otherwise it quits.
fn keep_running_in_tray(app: &AppHandle) -> bool {
    app.state::<AppState>().session.lock().unwrap().as_ref().is_some_and(|u| u.auto_sync)
}

#[tauri::command]
fn list_recordings(state: State<'_, AppState>) -> CmdResult<Vec<RecordingRow>> {
    let user = current_user(&state)?;
    state.db.lock().unwrap().list_recordings(user.id).map_err(err)
}

/// Desktop-only rename; never pushed to the device. Empty clears it.
#[tauri::command]
fn rename_recording(state: State<'_, AppState>, device_id: String, id: String, name: String) -> CmdResult<()> {
    let user = current_user(&state)?;
    let name = name.trim();
    let value = if name.is_empty() { None } else { Some(name) };
    state
        .db
        .lock()
        .unwrap()
        .set_recording_display_name(user.id, &device_id, &id, value)
        .map_err(err)
}

/// Explicit delete from the device (the device must be in sync mode).
/// The library copy is kept.
#[tauri::command]
async fn delete_from_device(state: State<'_, AppState>, device_id: String, id: String) -> CmdResult<()> {
    let user = current_user(&state)?;
    let row = state
        .db
        .lock()
        .unwrap()
        .get_device(user.id, &device_id)
        .map_err(err)?
        .ok_or("Unknown device.")?;
    let device = device_client(&state, &user, &row)?;
    let info = device.info().await.map_err(|_| "Device not reachable. Press S on the Cardputer.".to_string())?;
    if info.device_id != device_id {
        return Err("A different device answered at that address.".into());
    }
    match device.delete(&id).await {
        Ok(_) => {}
        Err(e) if e.code() == Some("not_found") => {}
        Err(e) if e.code() == Some("locked") => return Err("The recording is locked on the device.".into()),
        Err(e) => return Err(e.to_string()),
    }
    state.db.lock().unwrap().set_recording_on_device(user.id, &device_id, &id, false).map_err(err)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_autostart::init(MacosLauncher::LaunchAgent, Some(vec!["--hidden"])))
        .setup(|app| {
            let dir = app.path().app_data_dir()?;
            std::fs::create_dir_all(&dir)?;
            let db = Db::open(&dir.join("cprec.sqlite"))?;
            let remembered = db
                .setting(REMEMBERED_USER)?
                .and_then(|id| id.parse::<i64>().ok())
                .and_then(|id| db.user_by_id(id).ok().flatten());
            app.manage(AppState {
                db: Mutex::new(db),
                session: Mutex::new(None),
                host: Mutex::new(None),
                syncing: Mutex::new(HashSet::new()),
                nearby: Mutex::new(HashMap::new()),
                pairing: tokio::sync::Mutex::new(None),
                presence: Mutex::new(Presence::default()),
                mdns: Mutex::new(None),
            });
            let handle = app.handle().clone();
            if let Some(user) = &remembered {
                start_session(&handle, &app.state::<AppState>(), user);
            }

            build_tray(app)?;
            match discovery::start() {
                Ok((daemon, events)) => {
                    *app.state::<AppState>().mdns.lock().unwrap() = Some(daemon);
                    tauri::async_runtime::spawn(discovery_worker(handle.clone(), events));
                }
                Err(e) => eprintln!("mDNS discovery unavailable: {e}"),
            }

            // Started at login: stay in the tray if someone is logged in.
            let hidden = std::env::args().any(|a| a == "--hidden");
            if !(hidden && remembered.is_some()) {
                show_main_window(&handle);
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                if window.label() == MAIN_WINDOW && keep_running_in_tray(window.app_handle()) {
                    api.prevent_close();
                    let _ = window.hide();
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            session,
            create_account,
            login,
            logout,
            set_library_path,
            list_devices,
            add_device,
            rename_device,
            set_device_address,
            remove_device,
            ble_scan,
            pair_start,
            pair_ready,
            pair_finish,
            pair_cancel,
            app_settings,
            set_auto_sync,
            set_stay_logged_in,
            set_start_at_login,
            present_devices,
            sync_device,
            sync_device_ble,
            list_recordings,
            rename_recording,
            delete_from_device,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
