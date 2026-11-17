# Changelog

## Unreleased (1.5.0-dev) - Sync

### Milestone 6: Bluetooth sync fallback and release

- Sync over Bluetooth when WiFi is unavailable: offered on the sync screen
  when joining fails or no network is set (`Enter`), and from Settings >
  Sync > Sync over Bluetooth. Only the paired desktop can connect; the
  `/v1` requests travel in the encrypted pairing channel.
- `/v1` request handling moved into `SyncHandler`, shared by the HTTPS
  server and the Bluetooth fallback.
- BLE notifications wait for free buffers instead of being dropped.
- Fix: the device rebooted while testing WiFi during pairing, or when
  pairing after a WiFi sync (WiFi modem sleep was off with Bluetooth on;
  the WiFi radio now also stays up for the whole of pairing).
- Fix: WiFi downloads ran out of heap (10 KB/s, then aborted). The 80 KB of
  audio buffers are now allocated only while recording or playing, sync
  reads through its own 16 KB buffer, and the HTTPS server keeps one TLS
  session. Measured: about 270 KB/s with 70 KB of heap to spare.
- `cardputer-adv-recorder-debug` build environment with serial logs on USB.
- Publish an app-only `-m5launcher.bin` next to the full image, for installing
  from M5Launcher.

### Milestone 5: hands-free sync

- Announce `_cprec._tcp.local` (TXT `id`) over mDNS while in sync mode.
- Add Auto-Sync Charging (Settings > Sync, default On): starts sync mode on
  external power when there are unsynced recordings, with a 10-minute
  cooldown; `Esc` pauses it until unplugged.
- End sync mode after 3 minutes (automatic) or 10 minutes (`S`) without a
  desktop request.
- Show USB power and the number of new recordings in the library header.

### Milestone 4: pairing and mutual TLS

- Add Settings > Sync: Pair with Desktop, Paired With, WiFi, Delete After
  Sync, Forget Pairing.
- Pair over Bluetooth LE (NimBLE): numeric comparison with a committed
  nonce, AES-256-GCM session, WiFi provisioning from the desktop.
- Serve `/v1` over HTTPS on port 8443 with mutual TLS: self-signed device
  certificate, only the paired desktop's certificate accepted.
- Remove the build-time WiFi credentials and the plain HTTP server.

### Milestones 1 and 2: identity and WiFi data path

- Write a `cprc` RIFF chunk (recording id, device id, start time) into every
  new recording; playback reads it without seeking and older files still play.
- Keep a sync index in `RECORDER.SYN` with stable ids, cached SHA-256 and acks.
- Generate a per-device P-256 key pair on first boot; device id is derived
  from it.
- Add a dev sync mode (`S` in the library): WiFi from build flags and a plain
  HTTP `/v1` server with info, time, manifest, Range downloads, sha256, ack,
  delete and done.
- Add the `Delete After Sync` setting (default Off; locked files are kept).
- Pin M5GFX to 0.2.27 because 0.2.22 is no longer in the PlatformIO registry.

## 1.4.0 - Power, settings, and playback speed

- Remove the unused `Idle Sleep WIP` setting.
- Add `Triple-Press Wake` as an optional Screen Saver wake guard.
- Add configurable low battery auto-save for active recordings.
- Simplify the Help menu and remove visible WIP settings.
- Add settings reset confirmation and show the current firmware version in
  settings.
- Add live playback speed control with `[` and `]`.
- Add a configurable playback seek step with a 5-second default.

## 1.3.0 - Playback controls and file management

- Add pause and resume during playback.
- Add 10-second seek backward and forward during playback.
- Keep volume controls on Up/Down and use `Esc` to stop playback.
- Show `PAUSED` in the playback UI and dimmed playback screen.
- Keep wake-first-key behavior for dimmed or black screen playback.
- Add library file rename.
- Add recording lock state with delete protection.
- Add delete confirmation for unlocked recordings.
- Sort recordings by filename descending.
- Add a paged help screen opened with `H` from the library.

## 1.2.0 - App structure and release packaging

- Split the recorder application implementation into focused app modules for
  recording flow, playback flow, capture buffering, UI, settings, screen saver,
  and file browsing.
- Keep `RecorderApp` as the public application entry point while reducing the
  main implementation file size.
- Simplify release packaging to publish one complete flash image plus
  `SHA256SUMS.txt`.
- Document complete release image flashing at offset `0x0000`.

## 1.1.0 - Display settings and screen saver

- Change the header label to `RECORDER`.
- Show SD free space and selected-file size/duration in the library.
- Add persistent settings with brightness control.
- Add screen saver settings for home, recording, and playback states.
- Add Dimmed Standby and Black screen saver modes that keep recording and
  playback running.
- Add wake-first-key handling so the first input from a dimmed or black screen
  only restores the display.
- Mark stored but not-yet-implemented settings as WIP in the menu.

## 1.0.0 - Initial release

- Initial standalone release of Cardputer ADV Recorder.
- Stream 16 kHz mono PCM recordings to microSD.
- Browse, play, adjust volume, and delete recordings on the device.
- Add native WAV parsing and recording-name tests.
- Add automated firmware builds, tests, and release artifacts.
