# Cardputer ADV Recorder + Sync

A voice recorder for the M5Stack Cardputer ADV, and a desktop app that pulls
its recordings over your WiFi automatically. Pair once over Bluetooth; after
that, recordings move to your computer whenever the Cardputer is in sync mode
on the same network, encrypted and hash-verified. There is no cloud.

| Folder | What it is | Stack |
| --- | --- | --- |
| [`firmware/`](firmware/README.md) | Recorder firmware (fork of Moskic/cardputer-adv-recorder v1.4.0): 16 kHz mono WAV to microSD, plus pairing and sync | C++, PlatformIO, Arduino, ESP32-S3 |
| [`desktop/`](desktop/README.md) | Desktop app: local accounts, Bluetooth pairing, auto-sync from the system tray, library with playback | Tauri 2 (Rust) + React |
| [`tools/mock-device/`](tools/mock-device/README.md) | Fake device serving the sync protocol from a folder of WAVs, for developing the desktop app without hardware | Rust |

Each folder builds and tests on its own. The two programs talk only through
the protocols in [`docs/`](docs/).

## How it works

1. **Pair (once).** On the Cardputer: hold `G0` > Sync > Pair with Desktop.
   In the desktop app: Devices > Pair new device. Both screens show a
   six-digit code; confirm it on both. The desktop sends your WiFi network
   and password to the device over the encrypted Bluetooth link.
2. **Sync.** Press `S` in the Cardputer's library, or just plug it into USB
   (Auto-Sync Charging). It joins WiFi and announces itself on the network;
   the desktop app finds it and pulls new recordings, even with its window
   closed (it stays in the system tray).
   No shared network? Choose Sync over Bluetooth on both sides: slower
   (about 30 KB/s), but it needs no WiFi.
3. **Safety.** Each file is resumable, checked with SHA-256, saved to the
   library and recorded before the device is told it is safe. The device
   deletes a file only if you turned on Delete After Sync, and never a
   locked one.

Security: pairing uses numeric comparison with a nonce commitment over an
ECDH/AES-GCM session; sync is HTTPS with mutual TLS, each side pinned to the
other's key. One desktop user owns a device; pairing another needs the code
and a physical Enter press on the Cardputer.

## Where things are stored

| Where | What |
| --- | --- |
| Cardputer microSD | Recordings (`.WAV`, with a `cprc` chunk holding a stable recording id), `RECORDER.SYN` (sync index: ids, hashes, acks), `RECORDER.LCK` (locks) |
| Cardputer flash (NVS) | Settings, device key pair, the paired desktop and WiFi credentials |
| Desktop library folder | Synced audio as plain WAV files: `<library>/<device>/<YYYY-MM-DD>/<name>__<id8>.wav` (default `Music/Cardputer Sync/<user>`) |
| Desktop app data | `cprec.sqlite`: accounts (Argon2id hashes), devices, recording metadata, settings. Audio is not in the database |
| OS keychain | Each user's desktop key (pairing and TLS) |

## Status

Milestones from the [architecture doc](docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf):

| # | Milestone | State |
| --- | --- | --- |
| 1 | Recording identity (`cprc` chunk, sync index) | Done, verified on a device |
| 2 | WiFi data path | Done (HTTPS), about 270 KB/s on a device |
| 3 | Desktop shell: accounts, library, sync | Done, tested against the mock and a device |
| 4 | Bluetooth pairing, mutual TLS, WiFi provisioning | Done, verified on a device |
| 5 | Hands-free sync: mDNS discovery, tray worker, auto-sync when charging | Done; auto-sync on USB power seen on a device |
| 6 | BLE fallback transfer, installers, release image | Implemented; BLE sync not yet run on a device, installer CI not yet run |

The first device run (Cardputer ADV, Windows desktop) found and fixed six
problems that host tests could not show: memory, radio coexistence, a
certificate that changed on every start, and Windows Bluetooth scanning.
They are written up in [hardware findings](docs/hardware-findings.md). The
[hardware checklist](firmware/docs/hardware-test-checklist.md) covers what
is left to try. Deviations from the architecture doc are listed in
[`docs/ble-protocol.md`](docs/ble-protocol.md).

## Docs

- [How it is built and how it works](docs/architecture/overview.html) (detailed, with diagrams; open in a browser)
- [Architecture and `/v1` protocol](docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf) (original design)
- [BLE pairing protocol and sync fallback](docs/ble-protocol.md)
- [Testing without flashing the device](docs/dev-workflow.md), and
  [what the first device runs taught us](docs/hardware-findings.md)
- [Firmware](firmware/README.md), [desktop app](desktop/README.md), [mock device](tools/mock-device/README.md)

## License

[MIT](LICENSE)
