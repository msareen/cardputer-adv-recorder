# Desktop sync app

Tauri 2 (Rust) + React/TypeScript/Vite app that logs in a local user, pairs with
a Cardputer ADV Recorder, and pulls recordings over WiFi. See the
[architecture doc](../docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf).

Status: scaffolded from the Tauri React template (milestone 3 starting point).
It still shows the template's demo page; no sync logic yet.

## Run

```sh
npm install
npm run tauri dev      # app window with hot reload
npm run build          # type-check and build the frontend only
cd src-tauri && cargo check
```

Needs Node, Rust, and the Tauri prerequisites for your OS (on Windows, WebView2).

## Develop without hardware

Run [`tools/mock-device`](../tools/mock-device/README.md) and sync against it by
IP address (`127.0.0.1:8080`).

## Layout

- `src-tauri/`: Rust core. All device work goes here (HTTP client, BLE, mDNS,
  crypto, SQLite, sync worker).
- `src/`: React UI only. Talks to Rust through Tauri commands and events.
