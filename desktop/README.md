# Desktop sync app

Tauri 2 (Rust) + React/TypeScript/Vite app that logs in a local user, pairs
with a Cardputer ADV Recorder over Bluetooth, and pulls recordings over WiFi
with mutual TLS. See the
[architecture doc](../docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf)
and [BLE protocol](../docs/ble-protocol.md).

Status: milestone 6. Local accounts (Argon2id); a host key per user in the
OS keychain; Bluetooth pairing with code comparison and WiFi provisioning;
Change WiFi; sync over HTTPS pinned to the device key; mDNS discovery with a
background worker that syncs your devices as soon as they enter sync mode;
system tray (closing the window keeps syncing while auto-sync is on); stay
logged in and start at login; library grouped by device and date with
search, playback, desktop-only rename, show in folder, and delete on device;
Sync over Bluetooth for when the Cardputer cannot reach WiFi.

Settings: library folder, Auto-sync (per user), Stay logged in (opens
straight into that account; single-user computers only), Start at login
(starts hidden in the tray).

## Run

```sh
npm install
npm run tauri dev      # app window with hot reload
npm run build          # type-check and build the frontend only
cd src-tauri && cargo test     # engine, store, auth and protocol tests
```

Needs Node, Rust, and the Tauri prerequisites for your OS (on Windows, WebView2).

## Develop without hardware

Run [`tools/mock-device`](../tools/mock-device/README.md), then in the app go
to Devices > Developer: add an unpaired device, `127.0.0.1:8080`. Unpaired
devices use plain HTTP; paired ones always use HTTPS. Start the mock with
`--mdns` and the app's discovery worker finds and auto-syncs it. Without a
window:

```sh
cd src-tauri
cargo run --example sync_cli -- 127.0.0.1:8080 ./cli-library   # one sync
cargo run --example discover_cli -- 10                         # list announcing devices
cargo run --example ble_scan_cli -- 8 [--all]                  # what Bluetooth sees (pairing debug)
```

## Release

Pushing a `v*` tag runs `.github/workflows/release.yml`: the firmware
images, then installers for Windows (MSI, NSIS), macOS (universal DMG) and
Linux (AppImage, deb) attached to the same GitHub release. Signing is used
when these repository secrets exist, and skipped otherwise:

| Secret | For |
| --- | --- |
| `WINDOWS_CERTIFICATE`, `WINDOWS_CERTIFICATE_PASSWORD` | Authenticode: base64 `.pfx` and its password |
| `APPLE_CERTIFICATE`, `APPLE_CERTIFICATE_PASSWORD`, `APPLE_SIGNING_IDENTITY` | Developer ID Application certificate (base64 `.p12`) |
| `APPLE_ID`, `APPLE_PASSWORD`, `APPLE_TEAM_ID` | Notarization (app-specific password) |

Local installer for this OS: `npm run tauri build` (output in
`src-tauri/target/release/bundle/`).

## Layout

- `src-tauri/src/`
  - `pairing.rs`: BLE pairing protocol (commitment, ECDH/HKDF, code,
    AES-GCM channel, CBOR, framing), tested against a Rust reference device
    and vectors shared with the firmware.
  - `ble.rs`: btleplug scan and GATT transport.
  - `ble_sync.rs`: the BLE sync fallback, `/v1` tunnelled through the
    pairing channel as a `Device`; tested end to end with the sync engine.
  - `identity.rs`: host key in the OS keychain, self-signed client cert.
  - `tls.rs`: rustls config pinning the device key, with client auth;
    tested against a local mutual-TLS server.
  - `device.rs`: the `Device` trait and its HTTP(S) client (`/v1`, Range
    resume).
  - `discovery.rs`: mDNS browsing and `Presence`, which decides when an
    announcing device is due (once per appearance, failures retried after
    60 s). The worker, tray and close-to-tray live in `lib.rs`.
  - `sync.rs`: one sync pass: download to `.part`, verify SHA-256, rename into
    the library, write SQLite, then ack. Tests run it against an in-memory
    fake device with dropped connections, bad hashes and busy errors.
  - `db.rs`: SQLite (`users`, `devices`, `recordings`), every query scoped by
    user.
  - `auth.rs`, `library.rs` (file layout
    `<library>/<device>/<YYYY-MM-DD>/<name>__<id8>.wav`), `protocol.rs`.
  - `lib.rs`: Tauri commands and the `sync-progress` event.
- `src/`: React UI only (`Login`, `Library`, `Devices`, `Settings`). Audio
  plays from the library through Tauri's asset protocol, scoped at login to
  the user's library folder.

Data lives in the app data folder (`cprec.sqlite`). The default library is
`<Music>/Cardputer Sync/<username>`. Host keys are keychain entries named
`cprec-desktop` / `host-key-<user id>-<username>`; deleting one means
re-pairing every device of that user.

Platform notes: Windows needs Bluetooth LE and nothing else (the app only
makes outbound connections). macOS reads the Bluetooth permission text from
`src-tauri/Info.plist`. Linux needs BlueZ and a Secret Service (GNOME Keyring
or KWallet).
