# Hardware test checklist

- Cold boot and record a short clip immediately.
- Stop and confirm the file appears in the library.
- Confirm the library shows SD free space and selected-file size/duration.
- Open help with `H`, page through it, and return with `Enter` or `Esc`.
- Confirm Help shows only the simplified Library, Manage, Recording,
  Playback, and System pages.
- Play it and verify intelligible audio.
- Pause and resume playback several times.
- Confirm `Seek Step` offers 5, 10, 20, and 60 seconds and defaults to
  5 seconds.
- Seek backward and forward during playback and verify the progress changes
  by the configured seek step.
- Adjust volume during playback.
- Use `[` and `]` during playback and verify speeds change live through
  0.75x, 1.0x, 1.25x, 1.5x, and 2.0x.
- Verify pause/resume, seek, and volume controls still work after changing
  playback speed.
- Verify playback speed starts at 1.0x each time playback begins.
- Verify Dimmed Standby shows the current playback speed.
- Verify the first wake key from Dimmed Standby or Black does not change
  playback speed.
- Copy the WAV to a computer and verify its duration and PCM format.
- Rename a recording and confirm `.WAV` is preserved.
- Try a rename that conflicts with an existing file and confirm it is blocked.
- Try unsupported rename characters and confirm they are not accepted.
- Lock a recording and confirm it shows the lock marker.
- Confirm a locked recording cannot be deleted.
- Unlock the recording and confirm deletion requires confirmation.
- Delete an unlocked recording.
- Remove `RECORDER.LCK` and confirm the library still opens.
- Open settings with a long `G0` press.
- Change brightness and confirm it is restored after reboot.
- Confirm `Low Battery Save` offers Off, 1%, 5%, and 10%, defaults to 10%,
  and is restored after reboot.
- Confirm no WIP settings are shown.
- Confirm `Reset to Default` asks for confirmation before resetting settings.
- Confirm `Clear Recordings` shows the count on the first `Enter`, deletes only on the
  second, keeps locked files, and that `Esc` cancels.
- After a desktop sync, record: the file is `RECDDMMYYYY_HHMM.WAV` in local time and
  appears at the top of the library. After a power cycle (no sync), it is `RECnnnn.WAV`.
- Confirm settings shows the current firmware version.
- Enter `Screen Saver` settings and verify `When Home`, `While Recording`,
  `While Playing`, and `Triple-Press Wake` can be set independently.
- Verify `Dimmed Standby` and `Black` during recording do not interrupt
  recording.
- Verify `Dimmed Standby` and `Black` during playback do not interrupt
  playback.
- Verify the first key press from a dimmed or black screen wakes the display
  without stopping recording/playback, changing volume, or deleting a file.
- Verify the first key press from a dimmed or black screen does not pause,
  resume, seek, or stop playback.
- Turn on `Triple-Press Wake` and verify the same key must be pressed three
  times to wake from `Dimmed Standby` and `Black`.
- Verify a different key resets the triple-press wake count.
- Verify triple-press wake timeout requires starting the count again.
- With `Low Battery Save` enabled, simulate or test a low battery and confirm
  an active recording saves normally at the selected threshold.
- With `Low Battery Save` off, confirm low battery does not auto-stop a
  recording.
- Complete at least 20 record/play cycles.
- Record for at least ten minutes and verify stable memory and file growth.
- Test with no card and with less than 64 KiB free.
- Remove the card during recording and verify the app reaches an error screen
  without hanging.
- Repeat microphone-to-speaker transitions and listen for residual hiss.

## Pairing (Bluetooth)

Keep the serial monitor open: `[PAIR]` and `[SYNC]` lines log free heap.

- Settings > Sync > Pair with Desktop: "Scanning WiFi", then "Pair with
  desktop" with a two-minute countdown.
- Desktop: Devices > Pair new device > Search lists the Cardputer.
- Both screens show the same six digits. `Esc` on the device makes the
  desktop report "cancelled".
- Click Codes match, press `Enter`: the desktop lists WiFi networks the
  device saw. Finish with the right password: "WiFi OK", and Settings > Sync
  shows the desktop's user name and the SSID.
- Repeat with a wrong password: "WiFi failed", pairing kept. Devices >
  Change WiFi (device in pairing mode) fixes it without a code.
- Let pairing mode time out: it closes after two minutes.
- Pair a second desktop user: it needs the code and `Enter`, and replaces the
  first; the first desktop's sync now fails the TLS handshake.
- Forget Pairing needs two `Enter` presses; afterwards `S` asks to pair.
- After pairing, record and play a clip: Bluetooth is fully off.

## Sync

- Play a recording made by firmware 1.4.0 and one made by this build; both
  must play, and the new one must report its duration correctly.
- Press `S` in the library; confirm the device shows its `IP:8443` within
  15 seconds.
- `curl -k https://IP:8443/v1/info` without a client certificate must fail
  the handshake (mutual TLS).
- From the desktop, Sync now pulls every recording; each file has a 32-hex
  id, and the ids stay the same after a reboot and after a rename.
- Sync a 10-minute recording; compare the library file's SHA-256 with the
  file copied off the card.
- Pull the network mid-download, then sync again: the partial file resumes.
- Record the serial `free heap`, `min heap` and `KB/s` lines. Reference
  from the first run: 163 KB free at sync start, 138 KB serving, 70 KB at
  the lowest during downloads, 254-291 KB/s.
- Record and play a clip after a sync (audio buffers are allocated again);
  then start pairing after a WiFi sync without rebooting (modem sleep).
- Sync from the desktop app; confirm `Unsynced` drops to 0 and the device
  returns to the library after the desktop calls `/v1/done`.
- With `Delete After Sync` on, confirm acked files are deleted and locked
  files are kept.
- Press `Esc` during a download; WiFi turns off and the desktop resumes the
  partial file on the next sync.
- Confirm `R` does nothing in sync mode and that recording works normally
  after leaving it.

## Bluetooth sync fallback

Use the debug build (`platformio run -e cardputer-adv-recorder-debug
--target upload`) so serial logs reach USB.

- Pairing: after entering the WiFi password on the desktop, the device
  shows "Testing WiFi" then the result, and does not reboot (WiFi with
  Bluetooth on needs modem sleep).
- Set a wrong WiFi password (Change WiFi), press `S`: after 15 s the device
  shows "Sync over Bluetooth?". `Enter` starts BLE sync; on the desktop,
  Devices > Sync over Bluetooth shows an estimate, then syncs.
- Settings > Sync > Sync over Bluetooth starts it directly.
- Bluetooth only: pair (or Change connection) with Bluetooth only chosen on
  the desktop. The device closes pairing at once with "Bluetooth only: press
  S to sync", and Settings > Sync shows WiFi "BT only". `S` goes straight to
  BLE sync (no WiFi join); the desktop's Sync now syncs over BLE. Change
  connection > WiFi restores WiFi sync.
- Record the serial `KB/s` for a long recording over BLE (expected
  30-80 KB/s) and the `min heap`.
- Walk out of range mid-download: the desktop reports the link lost; Sync
  over Bluetooth again resumes the partial file.
- A second desktop (another user or computer) gets "paired with another
  desktop" and cannot read anything.
- `Esc` stops BLE sync and returns to the library; auto-sync never starts
  the BLE fallback by itself.

## Hands-free sync

- With the desktop app open (auto-sync on), press `S`: the device shows
  "on network" in Devices and syncs without clicking Sync now, then returns
  to the library.
- `dns-sd -B _cprec._tcp` (macOS) or the desktop's
  `cargo run --example discover_cli` lists the device only while in sync
  mode.
- Press `S` with the desktop app closed: sync mode ends by itself after
  10 minutes.
- Plug USB into a computer: the header shows `USB`. Record a clip, leave the
  device idle 15 s: auto-sync starts, shows `AUTO SYNC`, and ends after the
  desktop syncs (or after 3 minutes with no desktop).
- After an auto-sync, it does not start again for 10 minutes. `Esc` during
  an auto-sync stops it until USB is unplugged and replugged.
- Wall charger (no data): check whether the voltage rule (4.15 V) triggers
  auto-sync on a nearly full battery; note how long a charge takes to reach
  it.
- Turn Auto-Sync Charging off: no automatic start.

Automated builds cannot verify acoustic quality, SD hot removal, speaker
routing, or battery calibration.
