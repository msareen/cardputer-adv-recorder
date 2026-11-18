# Open questions

Decisions parked for later. Each says what happens today and the options.

## Clear Recordings and locked files (firmware)

Settings > Clear Recordings deletes every **unlocked** recording after a
second `Enter`. Locked recordings (shown with `*` in the library; `Left`
toggles the lock) are kept, as with single delete.

Found on hardware (November 2026): when every recording is locked, nothing
happens. The firmware sets the message "All recordings are locked." (and
"No recordings to clear." for an empty card), but **the Settings screen does
not draw `message_`**, so the user sees no feedback at all. Locks are also
easy to set by accident, since `Left` is a navigation key.

To decide:

1. Should Clear Recordings delete locked recordings too? Proposal: yes, with
   the count in the confirmation ("Delete 12 (5 locked)? ENTER again"), since
   the second `Enter` is already the safety step. Alternative: keep skipping
   them.
2. Either way, show the outcome on the Settings screen (value column
   `ALL LOCKED` / `NONE`, or draw `message_` in the footer).
3. Should locking move off `Left`, so it is not toggled while navigating?

Code: `RecorderApp::clearAllRecordings` (`firmware/src/app/file_browser.cpp`),
`kSettingClearRecordings` in `firmware/src/app/recorder_settings.cpp`.

## Recording dates without a clock (firmware + desktop)

The Cardputer ADV has no battery-backed clock. The desktop sets the time (and
time zone) on every sync; it is lost at power-off or reboot.

Today, with the clock set: files are named `RECDDMMYYYY_HHMM.WAV` in local
time and the `cprc` chunk holds the start time. Without it: files are named
`REC0001.WAV`, the chunk has no start time, and the desktop files the
recording under the **sync** day ("time unknown"), so day views and agent day
summaries put it on the wrong day.

Options (can be combined):

1. **NTP at power-on (proposed):** join the paired WiFi briefly at boot, get
   the time, switch WiFi off; remember the time zone in NVS. Radios stay off
   while recording.
2. **Back-date at sync (proposed fallback):** store milliseconds since boot
   when there is no clock; when a sync sets the clock in the same power-on
   session, compute the real start, fix the chunk/index and rename the file.
   Fails only if the device was powered off in between.
3. **Hardware RTC** (e.g. an M5Stack RTC unit on Grove): survives power-off
   with no WiFi; needs the module and a driver.

## Remote agents (desktop)

The agent MCP server listens on `127.0.0.1` only, so cloud chatbots cannot
reach it. See "Open questions" in `docs/agent-integration.md` (tunnel or
relay plus OAuth, or a hosted mirror).
