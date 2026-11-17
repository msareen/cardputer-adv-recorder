# Hardware findings (first runs on a Cardputer ADV)

Milestones 1 to 5 were written and host-tested before any of the sync code
ran on a device. The first session on a real Cardputer ADV (2026-11-17)
found six bugs that no host test could have caught. This page records each
one (symptom, evidence, cause, fix) and the rules that follow from them, so
the next change does not reintroduce them.

Result at the end of the session: pairing over Bluetooth, then a WiFi sync
over mutual TLS of 6 recordings (4.8 MB) at 254-291 KB/s, every file
SHA-256 verified, lowest free heap 70 KB.

## How it was debugged

- **Serial logs need the debug build.** The `m5stack-stamps3` board does not
  set `ARDUINO_USB_CDC_ON_BOOT`, so the release build's `Serial` goes to the
  UART pins and nothing arrives on USB. `cardputer-adv-recorder-debug`
  (same firmware, CDC on boot) fixes that:
  `platformio run -e cardputer-adv-recorder-debug --target upload`.
- **The firmware is quiet when idle.** It prints nothing at boot or in the
  library; the first lines come from pairing (`[PAIR]`) and sync (`[SYNC]`).
  An empty log is not a broken connection.
- **Opening the port without resetting the chip**: set DTR and RTS low before
  opening (pyserial `s.dtr = False; s.rts = False`). `RTS` high resets the
  ESP32-S3.
- **Decoding a crash**: copy the `Backtrace:` addresses and run
  `xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/<env>/firmware.elf <addrs>`
  (in `~/.platformio/packages/toolchain-xtensa-esp32s3/bin`). Check that the
  log's `ELF file SHA256` matches the ELF you decode with.
- **What the PC sees over Bluetooth**:
  `cargo run --example ble_scan_cli -- 8 [--all]` in `desktop/src-tauri`.
  Run it in a fresh process each time; Windows caches advertisement data per
  device, so a second scan in the same process can show data the first one
  never received.
- **Harmless lines**: `open(): /sd/RECORDER.LCK does not exist` (no locked
  files yet) and `wifi:timeout when WiFi un-init` (WiFi shutting down while
  Bluetooth shares the radio) appear in normal operation.

## 1. The desktop could not find the Cardputer

- **Symptom**: Pair new device listed nothing, although the Cardputer was in
  pairing mode a few centimetres away.
- **Evidence**: `ble_scan_cli` saw the Cardputer (name, service UUID, device
  id). A scan filtered on the service UUID, run alone, saw only a truncated
  name ("Cardpute") and no manufacturer data.
- **Cause**: the device id is in the scan response, which does not repeat the
  service UUID. Windows applies an advertisement filter to each packet
  separately, so it dropped the scan response, and the desktop skipped a
  device without an id.
- **Fix**: scan without a filter and match on our manufacturer data
  (`desktop/src-tauri/src/ble.rs`).
- **Rule**: never filter BLE scans by service on Windows when anything we
  need is in the scan response.

## 2. Pairing rebooted the Cardputer (twice, two causes)

- **Symptom**: after the WiFi password was sent, the desktop reported
  `HRESULT(0x800704C7) The operation was canceled by the user` (Windows'
  wording for a dropped BLE link). The device had rebooted.
- **Evidence**: `abort()` in `coex_core_enable`, once from `wifi_hw_start`
  (WiFi starting while BLE ran) and once from `esp_bt_controller_enable`
  (BLE starting while WiFi ran).
- **Cause**: WiFi and Bluetooth share one radio, and ESP-IDF aborts when one
  of them starts while WiFi runs without modem sleep. WiFi sync calls
  `WiFi.setSleep(false)` for throughput, and the Arduino core *remembers*
  that setting and reapplies it every time the station starts, even after
  `WIFI_OFF`. So pairing worked on a freshly booted device and crashed after
  any WiFi sync.
- **Fix** (`src/hardware/wifi_service.cpp`, `src/app/pairing_flow.cpp`):
  `begin()` sets `WiFi.setSleep(btStarted())`; `scan()` sets sleep on; `end()`
  restores sleep on. During pairing the WiFi radio is started once by the
  network scan and stays up until pairing closes, so no radio starts while
  the other is running in a fragile state.
- **Rule**: modem sleep must be on whenever Bluetooth is on, and the setting
  is global and sticky. Reset it when WiFi stops.

## 3. The desktop's certificate changed on every start

- **Symptom**: after pairing, every HTTPS connection failed on the device
  with `mbedtls_ssl_handshake returned -0x2700`
  (`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`).
- **Cause**: the device stores the desktop's self-signed client certificate at
  pairing and, through `esp_https_server`, trusts it as the only CA. mbedtls
  accepts a self-signed (non-CA) client certificate only if it is
  byte-for-byte in the trust list. The desktop rebuilt its certificate from
  the keychain key on every launch, with a random serial and a randomised
  ECDSA signature: same key, different bytes. The host tests never noticed
  because they run in one process.
- **Fix**: the certificate is deterministic (`desktop/src-tauri/src/identity.rs`):
  serial = SHA-256("cprec host cert" | public key), fixed validity dates,
  and RFC 6979 deterministic ECDSA (signing through p256 as an rcgen remote
  key). A test checks that two builds from the same key are identical.
- **Rule**: anything one side pins must be reproducible on the other side.
  Devices paired before this fix need Forget Pairing and a fresh pairing
  (re-pairing a known host only changes WiFi and keeps the old certificate).

## 4. Downloads ran out of memory

- **Symptom**: the first download crashed the device; after that fix,
  downloads ran at 10-13 KB/s and aborted after 130-230 KB. Lowest free heap:
  5.7-7.6 KB.
- **Causes**:
  1. The new `HttpReply` (`src/sync/sync_server.cpp`) held back the first
     write of every response so short JSON replies could carry
     Content-Length. For a download that first write was a 32 KB chunk;
     copying it threw `std::bad_alloc` (`operator new` in the backtrace).
  2. The heap was simply too small. The StampS3 has no PSRAM. 80 KB of audio
     buffers were static, a TLS session takes about 40-60 KB (the prebuilt
     mbedtls has fixed 16 KB record buffers each way and cannot be tuned from
     the Arduino build), and the server allowed two sockets. With 73 KB free
     while serving, one session plus WiFi transmit buffers left almost
     nothing, and lwIP stalled.
- **Fix**:
  - Only writes up to 1 KB are held back.
  - The 80 KB of audio buffers moved to the heap
    (`src/app/memory.cpp`): allocated when recording or playback starts,
    released when sync or pairing starts. States guarantee audio is idle
    then. Static RAM fell from 141 KB to 59 KB.
  - Sync reads the SD card through its own 16 KB buffer (`syncScratch_`),
    allocated for the sync and freed after it.
  - `max_open_sockets = 1`, so only one TLS session can exist.
- **Result**: free heap at sync start 98 KB → 163 KB, while serving 73 KB →
  138 KB, lowest during transfers 70 KB; throughput 254-291 KB/s.
- **Rule**: on this board, memory for audio and memory for the radios must
  take turns. Do not add static buffers; allocate in the state that needs
  them and free on the way out.

## 5. BLE notifications could be lost silently

Found while writing the Bluetooth sync fallback, before it ran on hardware:
`NimBLECharacteristic::notify()` (NimBLE-Arduino 1.4.3) ignores the return
code of `ble_gattc_notify_custom`, so a fragment is dropped when the host is
out of buffers. That is harmless for short pairing messages but would corrupt
a file transfer. `BleService::send` now calls `ble_gattc_notify_custom`
directly and retries until it succeeds (or the link drops).

## 6. A stale connection after a reboot looks like a hang

After the device rebooted mid-download, the desktop showed "Syncing…" with
no progress: its TCP connection was dead but not closed, and it waits up to
the 90 s read timeout before resuming (the timeout is long because hashing a
large file on the device takes about 20 s before it answers). It then resumes
the partial file with a Range request. Clicking Sync now does not help while
the old sync is still in flight.

## Memory budget (ESP32-S3, no PSRAM, 320 KB internal RAM)

| | Free heap |
| --- | --- |
| Idle in the library (audio buffers not allocated) | ~163 KB |
| Recording or playing (audio buffers allocated) | ~83 KB |
| WiFi sync, serving, no client | ~138 KB |
| WiFi sync, during a download (one TLS session) | ~70 KB lowest |
| Pairing: WiFi radio and BLE both up | 50 KB at start, ~41 KB lowest |

| Consumer | Size | Where |
| --- | --- | --- |
| Audio buffers | 80 KB (10 x 4096 x 16-bit) | heap, recording and playback only |
| TLS session (server, mutual auth) | ~40-60 KB | heap, during sync |
| WiFi station + HTTPS server task | ~25 KB | heap, during sync |
| Sync SD read buffer | 16 KB | heap, during sync |
| BLE (NimBLE host, one connection) | ~30 KB | heap, pairing and BLE sync |

## Rules this leaves

1. Every firmware change that touches radios, TLS or buffers needs a device
   run with the debug build; read `free heap`, `min heap` and `KB/s` lines.
2. Modem sleep stays on whenever Bluetooth is on (see 2).
3. Pinned material (certificates, keys) must be byte-stable across restarts
   (see 3).
4. Audio and radios take turns for memory; no new static buffers (see 4).
5. Check return codes of BLE sends (see 5).
6. Windows BLE scans: no service filter (see 1).
