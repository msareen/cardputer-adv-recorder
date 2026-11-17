# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repo shape

Three independent projects. They share only the `/v1` sync protocol, specified in `docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf` (read it with `pdftotext -layout`; the Read tool cannot render PDFs here). That PDF is the source of truth for the sync feature: milestones, protocol, pairing, data model.

- `firmware/`: Cardputer ADV recorder firmware (C++, PlatformIO, Arduino, ESP32-S3). Forked from Moskic/cardputer-adv-recorder v1.4.0 (MIT). Only the ADV is supported, not the original Cardputer.
- `desktop/`: Tauri 2 + React/TS sync app (local accounts, BLE pairing, mutual-TLS sync, mDNS auto-sync from the tray, library).
- `tools/mock-device/`: standalone Rust fake device serving `/v1` from a folder of WAVs. Exists so the desktop app can be developed without hardware.

Work is planned by the PDF's milestones (1 identity: `cprc` chunk + `/RECORDER.SYN` index, 2 WiFi data path, 3 desktop shell, 4 pairing/mTLS, 5 hands-free sync, 6 fallback/release); 1 to 6 are implemented (6's installer CI has not run yet), but no firmware change has been verified on a Cardputer yet (status table in the root `README.md`; device checks in `firmware/docs/hardware-test-checklist.md`). When a `/v1` endpoint changes, update the PDF's protocol, the mock device and the firmware together. The BLE wire format is specified in `docs/ble-protocol.md`; it deviates from the PDF by adding a nonce commitment (hello/hello/reveal), by keeping BLE off during WiFi sync (no Status characteristic; discovery is mDNS only), and by carrying the BLE fallback's file bytes in Control messages (no Data characteristic).

## Commands

Firmware (run from `firmware/`; `pio` is in `~/AppData/Roaming/Python/Python312/Scripts`, not on PATH by default):
```sh
platformio run -e cardputer-adv-recorder                    # build
platformio run -e cardputer-adv-recorder --target upload    # flash
platformio run -e cardputer-adv-recorder-debug --target upload   # same firmware, Serial on USB
platformio device monitor                                   # serial, 115200 (debug build only over USB)
platformio test -e native-tests                             # host tests (Unity)
platformio test -e native-tests -f test_sync                # one test suite (test_wav, test_sync, test_pairing)
```
Native tests need host gcc. On this machine it is MinGW-w64 copied to `D:\tools\mingw64` (prepend `/d/tools/mingw64/bin` to PATH): the WinGet install path contains a space (the username) and its linker breaks on that, even through a junction or 8.3 path. The device build compiles as gnu++11, so avoid brace-initializing structs that have default member initializers (native tests use gnu++17 and will not catch it).
Before a firmware PR: clean build, build, native tests (see `firmware/CONTRIBUTING.md`). Audio or storage changes also need `firmware/docs/hardware-test-checklist.md`. Release CI runs `firmware/scripts/package-release.sh` on `v*` tags (full image plus an app-only `-m5launcher.bin`), then builds desktop installers (signed only when the signing secrets exist; see `desktop/README.md`). The release build's `Serial` goes to the UART pins, not USB (no `ARDUINO_USB_CDC_ON_BOOT`), so use the debug environment to read logs over USB.

Desktop (from `desktop/`):
```sh
npm install
npm run tauri dev           # app with hot reload
npm run build               # tsc + vite build (frontend only)
cd src-tauri && cargo test  # sync engine (FakeDevice), pairing (reference device in Rust), mutual TLS (local server), db, auth
cd src-tauri && cargo test -- --ignored keychain   # touches the real OS keychain (throwaway entry)
cd src-tauri && cargo run --example sync_cli -- 127.0.0.1:8080 ./cli-library   # headless sync, e.g. against the mock
cd src-tauri && cargo run --example discover_cli -- 10                         # list devices announcing over mDNS
cd src-tauri && cargo run --example ble_scan_cli -- 8 [--all]                  # debug BLE pairing discovery (device in Pair with Desktop)
```
`tauri dev` serves the frontend on port 1420; a stale Vite from an earlier run blocks it ("Port 1420 is already in use").

Mock device (from `tools/mock-device/`):
```sh
cargo run -- --dir ./sdcard --port 8080 [--fault drop-mid|hash-mismatch|busy|low-battery] [--delete-after-sync] [--mdns]
```
No linters are configured.

## Firmware architecture

- `RecorderApp` (`include/recorder/recorder_app.h`, `src/recorder_app.cpp`) is one class owning everything: a `State` enum (`kBrowsing`, `kRecording`, `kSaving`, `kPlaying`, `kSettings`, `kRename`, `kHelp`, `kError`, `kSyncing`, `kPairing`) driven from `update()`. Its methods are split across `src/app/*.cpp` by concern (`recording_flow`, `playback_flow`, `capture_pipeline`, `file_browser`, `recorder_settings`, `recorder_ui`, `screen_saver`, `sync_flow`, `ble_sync_flow`, `pairing_flow`), not by class. Hardware access goes through `src/hardware/*_service.cpp` (board, audio, power, storage, wifi, ble).
- Recording streams through a pool of buffers (`audioBuffers_`, 10 x 4096 samples = 80 KB) and a dedicated FreeRTOS writer task with free/ready queues. The StampS3 has no PSRAM, so heap is the main constraint: the buffers are heap-allocated when recording or playback starts and released when sync or pairing starts (`app/memory.cpp`); sync gets its own 16 KB SD read buffer (`syncScratch_`). Measured on hardware: one TLS session takes about 40-60 KB (prebuilt mbedtls has fixed 16 KB record buffers), so the HTTPS server allows one socket; with static audio buffers a download ran at 10 KB/s and aborted at 5.7 KB free, now about 270 KB/s with 70 KB to spare. Serial logs report free heap and KB/s.
- New recordings get a `cprc` chunk between `fmt ` and `data` (92-byte header, `media/recording_chunk`). `WavReader` reads it sequentially (no seek, which is unreliable on some cards) and still falls back to the chunk walker. `WavWriter::headerSize()` is 92 for new files, 44 for legacy; the save-size check uses it.
- Sync state on SD: `/RECORDER.SYN` (text, `SyncIndex`), written via `/RECORDER.SYT` then renamed. Same pattern as `/RECORDER.LCK`. Hooks in save/delete/rename keep it current; `reconcileSyncIndex()` runs at sync start for files changed outside the app. The manifest cursor bumps on add, rename, delete and ack (not on hash caching).
- Pairing (`kPairing`, Settings > Sync > Pair with Desktop): WiFi scan, then NimBLE GATT for up to 2 min. BLE callbacks only queue reassembled frames (`BleService`, mutex); the main loop runs the protocol, mbedtls does ECDH/AES-GCM, pure helpers (`sync/cbor`, `sync/pairing_math`) are host-tested. The peer (host key, id, name, cert DER, WiFi) is in NVS namespace `cprec_peer` (`PeerStore`). A new peer requires the Enter press in `kConfirm`. `btInUse()` is overridden in `ble_service.cpp` so the Arduino core does not free the BT controller memory at boot.
- `/v1` request handling is transport-agnostic: `SyncHandler` (`sync/sync_handler`) takes a `SyncRequest` and writes to a `SyncReply`. `SyncServer` adapts it to `esp_https_server`; the BLE fallback (`app/ble_sync_flow.cpp`) adapts it to encrypted Control messages on the main loop.
- Sync (`S`, only when paired): HTTPS on 8443 via `esp_https_server` with mutual TLS (`client_verify_cert_pem` = stored host cert), plus mDNS `_cprec._tcp` TXT `id` (`WifiService::advertise`). Handlers run on the httpd task, so `syncMutex_` guards `syncIndex_` and progress fields; the loop only draws (try_lock), watches `syncDoneRequested_`, and ends sync after 3 min (auto) / 10 min (manual) without a request (`syncLastRequestMs_`). Acks are recorded under the peer's host id. BLE is off during WiFi sync. WiFi with BLE on (the WiFi test in pairing) must keep modem sleep: `WiFi.setSleep(false)` with Bluetooth enabled aborts and reboots.
- BLE fallback (milestone 6, `SyncPhase::kWifiFailed` / `kBluetooth`): offered when joining WiFi fails or no SSID is set (manual sync only), and from Settings > Sync. Reuses the pairing handshake with `pairForSync_` (unknown hosts get `not_paired`, no `pair`/`set_wifi`), then `{t:"req"}` messages go to `bleSyncHandler_`. Downloads block the loop, so the progress callback draws. `BleService::send` retries `ble_gattc_notify_custom` until buffers free up (NimBLE's `notify()` drops silently).
- Auto-sync (`serviceAutoSync`, only in `kBrowsing`): external power is a heuristic (`PowerService::externalPowerLikely`: `HWCDC::isPlugged()` or battery >= 4.15 V; the ADV has no charge pin). Needs idle input 15 s and unsynced > 0; every `stopSync` sets a 10-min cooldown; `Esc` suppresses until unplug. `libraryUnsynced_` (header status) is refreshed by the index hooks.
- Settings persist in NVS via `Preferences` (namespace `recorder`); the device key pair lives in namespace `cprec_id`, the peer in `cprec_peer`.
- `native-tests` compiles only the pure files listed in `build_src_filter` (`wav_format`, `file_naming`, `recording_chunk`, `sync/sha256`, `sync/sync_index`, `sync/protocol`, `sync/cbor`, `sync/pairing_math`). New host-testable logic must avoid Arduino/SD/WiFi/BLE/mbedtls and be added there. Pairing primitives have vectors shared with `desktop/src-tauri/src/pairing.rs` (`shared_vectors_with_firmware`); change both together.
- Fork policy from the PDF: put new sync code in new files (`hardware/wifi_service`, `hardware/ble_service`, `sync/*`, `app/sync_flow`, `app/ble_sync_flow`, `app/pairing_flow`) with only small hooks into `RecorderApp`, so rebasing on upstream stays cheap. Radios stay off while recording or playing.

## Desktop architecture

Rust (`src-tauri/src`) does all device work; React (`src/`) is UI only, via Tauri commands (`lib.rs`) and events.

- Sync engine: `sync.rs` is generic over the `Device` trait, so tests use `FakeDevice`. `device.rs`: `HttpDevice::paired` = HTTPS with `tls.rs`'s pinned-key verifier and the host client cert; `HttpDevice::new` = plain HTTP for unpaired dev devices like the mock. `devices.public_key` set means paired.
- Ordering rule: the ack is sent last, after the file is hashed, renamed into the library and recorded in SQLite, so a crash never makes the device think a file is safe when it is not; an unacked row is re-acked on the next sync. The device cursor is stored only when every manifest entry succeeded.
- BLE fallback: `ble_sync.rs` (`BleDevice<C: ControlChannel>`, implements `Device`), command `sync_device_ble`; `run_sync_with` takes a connect future so HTTPS and BLE share the event flow. BLE scans must not use a service filter (Windows drops the scan response that holds the device id).
- Pairing: `pair_start` / `pair_ready` / `pair_finish` / `pair_cancel` hold a `Session<BleLink>` in `AppState`; `pairing.rs` is generic over `ControlChannel` and tested against a Rust reference device (`pairing::fake`). The host key per user is in the OS keychain (`identity.rs`), cached per login.
- Hands-free: `discovery.rs` (mdns-sd browse on a thread; `Presence` decides due devices: once per appearance, failures retried after 60 s) feeds `discovery_worker` in `lib.rs`, which calls the same `run_sync` as the Sync now command. `run_sync` emits `sync-started` / `sync-progress` / `sync-finished`, and the UI follows those events rather than its own calls. Tray and close-to-tray (while logged in with auto-sync), `--hidden` start (autostart plugin), `settings.remembered_user` for stay logged in.
- Storage: audio is plain WAV in the user's library folder (`<library>/<device>/<YYYY-MM-DD>/<name>__<id8>.wav`, partials in `<library>/.partial/`); SQLite (`%APPDATA%\com.cprec.desktop\cprec.sqlite`) holds users, devices, recording metadata, settings. Migrations use `PRAGMA user_version` (`Db::migrate`). That database on this machine holds the user's real account: back it up and restore it around any end-to-end test that launches the app.
- Cargo builds while `tauri dev` runs contend for the target-dir lock (and `tauri dev` rebuilds on every `src-tauri` change); run tests with `CARGO_TARGET_DIR=target-test cargo test` then.

## Mock device notes

Rescans its folder on every request and bumps the manifest cursor when the listing or an ack changes. Plain HTTP only (the desktop treats it as an unpaired dev device; no BLE or TLS mock). Recording ids are a placeholder hash of name and size, not the real `cprc` id. Files whose name contains `LOCKED` count as locked. `--fault drop-mid` sends half of each body then resets, so downloads under it never finish. `--mdns` announces it on loopback (the desktop's discovery enables loopback too).

## Conventions

- Git on this machine warns about LF to CRLF conversion; that is harmless.
- Patching files with Python on this machine: open them with `encoding="utf-8"` (the default is cp1252, which mangles `…`), and run scripts from a file rather than a bash heredoc (heredocs with quotes in them fail in this shell).
- Open an issue before large behavioral or hardware changes to the firmware; PRs must not assume original-Cardputer compatibility.
