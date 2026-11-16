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
`done`) on localhost, serving WAVs from a folder. It can inject faults: dropped
connections mid-download, hash mismatch, `busy`, `low_battery`. The desktop app
syncs against it by IP address (milestone 3), so no hardware is needed.

## 3. Real device (firmware)

Only for what the PC cannot show: heap, SD speed, radio coexistence, battery.
Build and flash with `platformio run --target upload`, then walk through
`firmware/docs/hardware-test-checklist.md`. During milestone 2, test the device
HTTP server with plain `curl` against the same endpoints the mock serves, so
mock and firmware are checked against one protocol.

## Keeping mock and firmware in agreement

The protocol is the contract. When an endpoint changes, update the architecture
doc, the mock device and the firmware in the same change.
