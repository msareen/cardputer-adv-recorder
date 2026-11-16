# Desktop sync app

Tauri (Rust) + React app that logs in a local user, pairs with a Cardputer ADV
Recorder, and pulls recordings over WiFi. Not scaffolded yet: see milestone 3 in
[the architecture doc](../docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf).

Planned layout:

- `src-tauri/`: Rust core (BLE, mDNS, HTTP client, crypto, SQLite, sync worker)
- `src/`: React UI

For development without hardware, run
[`tools/mock-device`](../tools/mock-device/README.md) and sync against it by IP
address.
