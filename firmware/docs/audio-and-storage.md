# Audio and storage

## Audio lifecycle

Cardputer ADV uses an ES8311 codec. Microphone and speaker operation are
mutually exclusive and are serialized by `AudioService`.

During capture, the firmware keeps M5Unified's two microphone slots occupied
and transfers completed blocks to a deeper application queue. A dedicated
low-priority task optionally runs the voice processor, updates the level
snapshot, and writes blocks to microSD.

## Capture gain and mic modes

The level comes from the ES8311 analog PGA (register 0x14), set by
`AudioService::startCapture` after `Mic.begin()`, with `Mic_Class` at unity
(`magnification` 2). M5Unified leaves the PGA at 0 dB; the recorder used to
make up 24 dB by multiplying the 16-bit samples, so recordings moved in steps
of 16 and the hiss floor was quantization noise.

The PGA is 24 dB, matching the old loudness and clipping point. Settings >
Mic Mode then chooses what happens to the samples (`kVoicePreset` in
`app/recording_flow.cpp`):

| Mode | Processing |
|---|---|
| Voice (default) | 100 Hz high-pass, +3 dB above 3 kHz, up to 9 dB of speech levelling, pauses 8 dB down |
| Natural | none; the microphone as captured |
| Original | the 1.4.0 capture, for comparison: PGA at 0 dB, samples multiplied by 16, peaks above -2.5 dBFS compressed 4:1 |

On the device, a 12 dB levelling ceiling sounded a little loud, and a Room
preset for distant voices (30 dB PGA, up to 18 dB of levelling, no expander)
mostly raised hiss, so it was dropped.

`VoiceProcessor` (`media/voice_processor`, host-tested in `test_voice`) runs
on the writer task. Its gain moves only on speech and slowly (0.5 s to fall),
since faster gain changes were heard as crackle on the device. The first
200 ms are faded in and not measured. The serial log (debug build) prints the
mode at capture start, and the final gain, noise floor, clipped and limited
sample counts when a Voice recording ends.

The file stays 16 kHz mono 16-bit. `Mic_Class` over-samples by 2 and its
clocking of the ES8311 has not been measured, so the rate was left alone.

Playback queues fixed-size mono 16-bit PCM blocks. A queued buffer must remain
unchanged until the speaker has consumed it.

## WAV finalization

Recording starts with a streaming WAV header. On normal stop, the writer seeks
back to repair the RIFF and data sizes, syncs the descriptor, and closes it.
An incomplete recording is discarded on capture or write failure.

The reader handles unknown RIFF chunks and does not require a fixed data
offset. Playback is limited to mono 16-bit PCM.

## File format: why WAV

Recordings stay 16 kHz mono 16-bit WAV (32 KB/s, about 115 MB an hour). WAV
came from upstream rather than a comparison; it was reviewed against
compressed formats in November 2026 and kept.

WAV fits this board: encoding costs nothing, `VoiceProcessor` works on the
PCM samples directly, the `cprc` chunk relies on RIFF readers skipping
unknown chunks, and a streaming header (size `UINT32_MAX`) lets the reader
fall back to the file size. The cost is size: sync time over WiFi and, much
more, over the BLE fallback.

| Format | Size vs WAV | Notes |
|---|---|---|
| WAV (PCM, current) | 1 | lossless, no encoder |
| IMA ADPCM in WAV | 1/4 | tiny encoder state, still RIFF and streaming; slight loss, fine for speech. Cheapest change if size matters |
| Opus in Ogg (16-24 kbps) | ~1/10-1/15 | best for speech; Ogg pages survive truncation, `cprc` could go in OpusTags. RAM is unproven (below) |
| AAC in MP4/M4A | ~1/8 | heavier than Opus for speech; an MP4 cut off before its `moov` atom does not play |

Opus was not adopted because it does not fit the memory budget with any
certainty. Free heap while recording is about 83 KB (see
`docs/hardware-findings.md`), and the StampS3 has no PSRAM. Espressif's
[`esp_audio_codec`](https://components.espressif.com/components/espressif/esp_audio_codec)
lists an Opus encoder but publishes no Opus memory or CPU figures; ESPHome's
[`micro-opus`](https://components.espressif.com/components/esphome/micro-opus)
reserves a 120 KB pseudostack per thread and says SILK encoding on the S3 is
real-time only with the fixed-point build. Playback would need a decoder too,
and none of this has been measured on the device. Whether `esp_audio_codec`
builds under this Arduino setup was not checked.

A compressed format would not improve quality: WAV is already lossless, and
quality is set by the microphone, the PGA, Mic Mode and the 16 kHz rate
(content up to 8 kHz). Opus would only make a higher sample rate cheap in
size, which helps only if the microphone delivers anything above 8 kHz.

If this is revisited, first build a test that encodes Ogg Opus on the device
and logs free heap and CPU. A format option would also touch the `.WAV`
assumptions in `app/file_browser.cpp`, `media/file_naming.cpp`,
`app/recorder_ui.cpp`, the `audio/wav` content type in `sync/sync_handler.cpp`
and `sync/sync_server.cpp`, the desktop's `library.rs`, `sync.rs` and
`Library.tsx`, the mock device, and the PDF's protocol and data model.

## microSD

Cardputer ADV microSD uses SCK 40, MISO 39, MOSI 14, and CS 12. The firmware
uses a conservative 10 MHz clock for reliable sustained writes. These values
belong in `StorageService` and should not be duplicated elsewhere.
