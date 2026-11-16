# Cardputer ADV Recorder + Sync

Two programs and a dev tool, each independent, in one repo:

| Folder | What it is | Stack |
| --- | --- | --- |
| [`firmware/`](firmware/README.md) | Voice recorder firmware for the M5Stack Cardputer ADV (fork of Moskic/cardputer-adv-recorder v1.4.0) | C++, PlatformIO, Arduino, ESP32-S3 |
| [`desktop/`](desktop/README.md) | Desktop app that pairs with the device and pulls recordings over the LAN | Tauri (Rust) + React |
| [`tools/mock-device/`](tools/mock-device/README.md) | Fake device serving the sync protocol from a folder of WAVs, for developing the desktop app without hardware | Rust |

The design for the sync feature is in
[`docs/architecture/`](docs/architecture/Cardputer-ADV-Recorder-Sync-Architecture.pdf).
The two sides talk only through the `/v1` HTTP protocol described there, so each
folder can be built and tested on its own.

## Testing without flashing the device

See [`docs/dev-workflow.md`](docs/dev-workflow.md).

## License

[MIT](LICENSE)
