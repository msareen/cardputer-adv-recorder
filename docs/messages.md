# Messages: what they mean

What the Cardputer and the desktop app say during pairing and sync, what
each message means, and what to do. The Cardputer screen has a **heading**
(large text) and a **line** under it (small grey text); results also stay on
the library screen after pairing or sync ends.

Keys: `S` sync, `Enter` confirm, `Esc` stop or cancel, `G0` (hold) settings.

## Cardputer: pairing (G0 > Sync > Pair with Desktop)

| Heading | Line | Meaning / what to do |
|---|---|---|
| Getting ready | Looking for WiFi networks... | Pairing mode is opening. The Cardputer lists nearby networks (about 2 s) so the desktop can offer them. It does this even if you will choose Bluetooth only: nothing is joined. |
| Ready to pair | On the desktop: Devices > Pair. | Waiting for the desktop over Bluetooth. Open the app, Devices > Pair new device (or Change connection) and Search. Closes after 2 minutes. |
| Pairing | Connecting... | The desktop found it and is setting up the encrypted link. |
| Same code on the desktop? | Compare the code. | New desktop only. If the six digits match the desktop, press `Enter` here and click Codes match there. If they differ, press `Esc`. |
| Pairing | Finish on the desktop. | You pressed `Enter`. Choose WiFi or Bluetooth only in the app. |
| Pairing | Connected. Finish on the desktop. | A desktop this Cardputer already knows (Change connection). No code needed. |
| Testing WiFi | Joining *network*... | WiFi was chosen. The Cardputer tries the network once (up to 15 s). |
| Saving | | Bluetooth only was chosen; it is saving the choice. |
| WiFi OK: *network* | Paired with *name*. | Done. Press `Enter` to close. `S` syncs over WiFi from now on. |
| WiFi failed: *network* | Paired with *name*. | The pairing is kept but the network did not work (wrong password, out of range, guest network with a login page). Use Change connection on the desktop: try again, or pick Bluetooth only. |
| Bluetooth only: press S to sync. | Paired with *name*. | Done, without WiFi. `S` always syncs over Bluetooth. |
| | Desktop disconnected. | The desktop closed the link before finishing. The Cardputer waits again; search again in the app. |

When pairing ends, one of these shows on the library screen:

| Message | Meaning / what to do |
|---|---|
| Pairing cancelled. | `Esc` was pressed. |
| Pairing timed out. | Nothing finished within 2 minutes. Start again on both sides. |
| Pairing failed: bad certificate. | The desktop's identity did not check out. Try again; if it repeats, restart the app. |
| Pairing failed: could not save. | The Cardputer could not write its settings memory. Restart it and pair again. |
| Bluetooth failed to start. | Usually low memory. Restart the Cardputer. |
| Device key unavailable. | The Cardputer's identity key could not be loaded. Restart; if it repeats, report it. |
| Pairing and WiFi forgotten. | Settings > Sync > Forget pairing was confirmed. Pair again to sync. |

## Cardputer: sync (`S`)

Before anything starts, `S` can refuse with one of these on the library screen:

| Message | Meaning / what to do |
|---|---|
| Pair first: G0 > Sync > Pair. | Not paired with any desktop yet. |
| Battery low: charge to sync. | Charge first; sync is blocked to protect recordings. |
| Not enough memory. Restart. | Restart the Cardputer and try again. |
| E1: microSD mount failed. | The SD card is missing or unreadable. |

### Over WiFi

| Heading | Line | Meaning / what to do |
|---|---|---|
| Joining WiFi | Joining *network*... | Connecting to the network saved at pairing. |
| Joining WiFi | Starting secure server... | Joined; starting the encrypted server. |
| *ip:8443* | Waiting for *name*... | Ready. The desktop syncs by itself (auto-sync) or click Sync now. |
| *ip:8443* | *file* and a progress bar | Transferring. |
| Sync over Bluetooth? | WiFi failed: *network* | The network could not be joined within 15 s. Press `Enter` to sync over Bluetooth instead, or `Esc` to stop. |

### Over Bluetooth

| Heading | Line | Meaning / what to do |
|---|---|---|
| Bluetooth sync | On the desktop: Sync now. | Bluetooth-only device: click Sync now on the desktop. |
| Bluetooth sync | On the desktop: Sync over Bluetooth. | WiFi device using the fallback: click Sync over Bluetooth on the desktop. |
| Bluetooth sync | Desktop connected. | The desktop is connected; files follow. |
| Bluetooth sync | Syncing over Bluetooth... | Transferring (about 30 KB/s). Keep the Cardputer near the computer. |
| Bluetooth sync | Desktop disconnected. | The link dropped. Click sync on the desktop again; a partial file resumes. |

The footer shows `SECURE SYNC` (WiFi, started by you), `AUTO SYNC` (WiFi,
started on external power) or `BLE SYNC`, and `Unsynced N` counts recordings
this desktop has not saved yet.

### When sync ends

| Message | Meaning / what to do |
|---|---|
| Sync done: N synced. | The desktop finished; N recordings are now safe on it. |
| No desktop connected. | Nobody synced in time (3 min auto, 10 min manual). Check the app is open and logged in, then press `S` again. |
| Sync stopped. | `Esc` was pressed (auto-sync then waits until you unplug). |
| WiFi lost. | The network dropped mid-sync. Press `S` again; partial files resume. |
| WiFi failed: change it from the desktop. | Auto-sync could not join the network. Use Change connection on the desktop. |
| WiFi start failed. | The radio did not start. Restart the Cardputer. |
| Secure server failed. / Could not build TLS keys. | Usually low memory. Restart the Cardputer. |
| Bluetooth sync failed. / Bluetooth failed to start. | Usually low memory. Restart the Cardputer. |

### Settings > Sync

| Row | Value | Meaning |
|---|---|---|
| Paired With | *name* / None | The desktop account this Cardputer syncs with. |
| WiFi | *network* | Syncs over WiFi. |
| WiFi | BT only | Paired as Bluetooth only: `S` syncs over Bluetooth. |
| WiFi | Not set | Not paired. |

## Desktop app

### Device card

| Shown | Meaning |
|---|---|
| `paired · TLS` | Paired over Bluetooth; WiFi sync is encrypted and pinned to this Cardputer. |
| `dev · HTTP` | An unpaired developer device (the mock), plain HTTP. |
| `Bluetooth only` | Paired without WiFi. Sync now goes over Bluetooth; no auto-sync. |
| `on network` | The Cardputer is announcing sync mode on this network right now. |
| Over Bluetooth: *size*, about N min | Estimate before a Bluetooth transfer starts. |
| *n* new · *n* renamed · *n* cleared from device · *n* failed | Result of the last sync. "Cleared from device" means delete-after-sync removed them from the SD card after they were saved here. |

### Errors

| Message | Meaning / what to do |
|---|---|
| The Cardputer is not waiting for Bluetooth sync... | It is not in Bluetooth sync. Press `S` on it (Bluetooth only), or Settings > Sync > Sync over Bluetooth, then try again. |
| This Cardputer is paired with another desktop. | Another account or computer owns it. Pair it again with this one (that replaces the other). |
| This Cardputer does not know this computer any more... (in Change connection) | Its pairing was forgotten on the device, or another desktop paired it. Compare the code and press `Enter` on the Cardputer to pair again; your library and history are kept. |
| That device is no longer in range. Scan again. | It stopped advertising (pairing timed out or was cancelled). |
| The device's key does not match what it advertised. | Something answered with the wrong identity. Try again; if it repeats, pair somewhere else. |
| the device did not answer in time | Bluetooth went quiet. Move closer and retry. |
| the device said: cancelled / timeout | `Esc` was pressed on the Cardputer, or its 2-minute window ran out. |
| the device said: not_paired | Bluetooth sync with a Cardputer that does not know this desktop. Pair first. |
| the device said: bad wifi / storage / bad certificate | Network name or password too long; it could not save; the identity check failed. |
| Bluetooth: no Bluetooth adapter found | Turn Bluetooth on in Windows. |
| Bluetooth: the device disconnected / the Bluetooth link was interrupted | The link dropped. Retry closer; partial files resume. |
| message failed authentication | An encrypted message did not verify. Retry; if it repeats, pair again. |
| Device not reachable. Press S on the Cardputer. | It is not in WiFi sync mode (or not on this network). |
| No address for this device yet. Edit it, or pair again. | It has never been seen on the network. Press `S` with the app open. |
| This device is already syncing. | A sync for it is already running (maybe an automatic one). |
| This device is set to Bluetooth only; it never joins WiFi. | Auto-sync skipped it. Use Sync now with the Cardputer in Bluetooth sync. |
| Choose a WiFi network (up to 32 bytes). / WiFi passwords are at most 63 characters. | Fix the WiFi form. |
| The recording is locked on the device. | Unlock it on the Cardputer before deleting it from the desktop. |

## Other Cardputer errors (E codes)

| Code | Meaning |
|---|---|
| E1 | microSD mount failed: card missing, loose or unreadable. |
| E2 | Not enough SD space. |
| E3 | No free filename. |
| E4 | The WAV file could not be created or finished. |
| E5 / E7 | The microphone did not start (or timed out starting). |
| E6 | Recording queue or writer task failed to start. |
| E8 | Writing the recording to the SD card failed. |
| E9 | Capture buffer overflow, or not enough memory (restart). |

Messages change with the firmware: when you add or reword one in
`firmware/src/app/*_flow.cpp` or `desktop/src-tauri/src`, update this file.
