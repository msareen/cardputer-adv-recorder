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

Settings > Mic Mode picks a preset (`kMicPresets` in `app/recording_flow.cpp`):

| Mode | PGA | Processing |
|---|---|---|
| Natural (default) | 24 dB | none; same loudness as the old firmware |
| Voice | 24 dB | 100 Hz high-pass, +3 dB above 3 kHz, up to 12 dB of speech levelling, pauses 8 dB down |
| Room | 30 dB | 80 Hz high-pass, up to 18 dB of speech levelling, pauses untouched |

`VoiceProcessor` (`media/voice_processor`, host-tested in `test_voice`) runs
on the writer task. Its gain moves only on speech and slowly (0.5 s to fall),
since faster gain changes were heard as crackle on the device. The first
200 ms are faded in and not measured. The serial log (debug build) prints the
mode and PGA at capture start, and the final gain, noise floor, clipped and
limited sample counts when a processed recording ends.

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

## microSD

Cardputer ADV microSD uses SCK 40, MISO 39, MOSI 14, and CS 12. The firmware
uses a conservative 10 MHz clock for reliable sustained writes. These values
belong in `StorageService` and should not be duplicated elsewhere.
