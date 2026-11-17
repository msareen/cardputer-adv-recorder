# Development workflow: test without redeploying

Flashing the Cardputer is the slowest step in the loop, so most work should be
verified on the PC first. Three layers, from fastest to slowest:

## 1. Host unit tests (firmware)

Pure C++ logic (WAV parsing, file naming, and later the `cprc` chunk, the
`/RECORDER.SYN` index and protocol encoding) is compiled for the PC and run
under Unity.

```sh
cd firmware
platformio test -e native-tests
```

Rule: new sync logic that does not touch Arduino, SD, WiFi or BLE goes in its own
`.cpp` file and is added to `build_src_filter` in the `native-tests` env.

## 2. Mock device

`tools/mock-device` (its own Rust project, independent of firmware and desktop) implements the device side of the `/v1` protocol
(`info`, `time`, `manifest`, `recordings/{id}` with Range, `sha256`, `ack`,
`done`) on localhost over plain HTTP, serving WAVs from a folder. The desktop
adds it as an unpaired dev device; real devices are paired over Bluetooth and
use HTTPS with mutual TLS. With `--mdns` it announces itself like a device in
sync mode, so the desktop's discovery worker auto-syncs it. It can inject
faults: dropped
connections mid-download, hash mismatch, `busy`, `low_battery`. The desktop app
syncs against it by IP address (milestone 3), so no hardware is needed.

## 3. Real device (firmware)

Only for what the PC cannot show: heap, SD speed, radio coexistence, battery,
and the real Bluetooth and TLS stacks. Every bug found on the first device
run was of this kind (see [hardware findings](hardware-findings.md)), so any
change to radios, TLS or buffers needs one.

Flash the debug build, which sends `Serial` to USB (the release build sends
it to the UART pins):

```sh
cd firmware
platformio run -e cardputer-adv-recorder-debug --target upload
platformio device monitor            # 115200; quiet until pairing or sync
```

Read the `free heap`, `min heap` and `KB/s` lines. To decode a crash, pass
the `Backtrace:` addresses to `xtensa-esp32s3-elf-addr2line -pfiaC -e
.pio/build/cardputer-adv-recorder-debug/firmware.elf`. If the desktop cannot
find the device, `cargo run --example ble_scan_cli -- 8` (in
`desktop/src-tauri`) shows what the PC receives over Bluetooth. Then walk
through `firmware/docs/hardware-test-checklist.md`, and flash the release
build again for normal use.

The pairing protocol is checked on the PC from both ends: the desktop runs it
against a reference device written in Rust (`pairing.rs`), and the firmware's
pure parts (CBOR, framing, HKDF, code) are tested against the same vectors.
The desktop's mutual-TLS client is tested against a local TLS server.

## Keeping mock and firmware in agreement

The protocol is the contract. When an endpoint changes, update the architecture
doc, the mock device and the firmware in the same change.
