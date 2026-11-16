# mock-device

A fake Cardputer ADV Recorder for developing the desktop app without hardware.
It serves the `/v1` sync protocol (plain HTTP, the dev profile) from a folder of
WAV files that stands in for the SD card.

```sh
cd tools/mock-device
cargo run -- --dir ./sdcard --port 8080
curl localhost:8080/v1/info
```

Put `.wav` files in the folder, even while it runs: the folder is rescanned on
every request and the manifest cursor bumps when it changes.

| Flag | Effect |
| --- | --- |
| `--fault none` | Behave correctly (default) |
| `--fault drop-mid` | Cut the connection halfway through every download |
| `--fault hash-mismatch` | Report a wrong SHA-256 |
| `--fault busy` | 409 `busy` on everything except `/v1/info` |
| `--fault low-battery` | 3% battery, 403 `low_battery` except on `/v1/info` |
| `--delete-after-sync` | Delete the file on ack, unless its name contains `LOCKED` |
| `--mdns` | Announce `_cprec._tcp.local` on loopback, like a device in sync mode; the desktop auto-syncs it if it was added as a dev device |

Not mocked yet: TLS, BLE, pairing, and the real `cprc` recording id (ids
are a placeholder hash of name and size, in the same 32-hex format). Download
reads the whole file into memory, which is fine for a dev tool.

`drop-mid` sends the first half of each response body, then resets the
connection, so a client keeps a partial file and must resume with `Range`.
Because every request is cut, a download never completes under this fault;
restart the mock without it to watch the resume finish.

End-to-end check with the desktop engine (no window needed):

```sh
cd desktop/src-tauri
cargo run --example sync_cli -- 127.0.0.1:8080 ./cli-library
```
