//! Local speech-to-text with sherpa-onnx (via sherpa-rs), Whisper models.
//!
//! The model is not bundled: the user points the app at a folder holding a
//! sherpa-onnx Whisper export (`*-encoder*.onnx`, `*-decoder*.onnx`,
//! `*-tokens.txt`), e.g. `sherpa-onnx-whisper-base.en` from
//! <https://github.com/k2-fsa/sherpa-onnx/releases/tag/asr-models>.
//! Whisper decodes at most 30 s at a time, so the audio is cut into
//! windows and the result is a WebVTT transcript with one cue per window.

use std::path::{Path, PathBuf};

use sherpa_rs::whisper::{WhisperConfig, WhisperRecognizer};

/// Sherpa's feature extractor and Whisper both expect 16 kHz mono.
pub const SAMPLE_RATE: u32 = 16_000;
/// Window length in seconds; Whisper's limit is 30.
pub const WINDOW_SECS: usize = 28;

#[derive(Debug, thiserror::Error)]
pub enum TranscribeError {
    #[error("Speech model folder not found: {0}")]
    NoModel(String),
    #[error("{0}")]
    Model(String),
    #[error("Cannot read audio: {0}")]
    Audio(String),
}

type Result<T> = std::result::Result<T, TranscribeError>;

/// Files of a sherpa-onnx Whisper export.
#[derive(Debug, PartialEq)]
pub struct ModelFiles {
    pub encoder: PathBuf,
    pub decoder: PathBuf,
    pub tokens: PathBuf,
}

/// Finds the three model files in `dir`, preferring int8 variants (smaller,
/// faster, same quality for speech).
pub fn find_model(dir: &Path) -> Result<ModelFiles> {
    let entries: Vec<String> = std::fs::read_dir(dir)
        .map_err(|_| TranscribeError::NoModel(dir.display().to_string()))?
        .filter_map(|e| e.ok()?.file_name().into_string().ok())
        .collect();
    let pick = |part: &str, suffix: &str| -> Option<PathBuf> {
        let mut hits: Vec<&String> = entries.iter().filter(|n| n.contains(part) && n.ends_with(suffix)).collect();
        hits.sort_by_key(|n| (!n.contains("int8"), n.len()));
        hits.first().map(|n| dir.join(n))
    };
    let missing = |what: &str| TranscribeError::Model(format!("{} has no Whisper {what} file (sherpa-onnx export expected).", dir.display()));
    Ok(ModelFiles {
        encoder: pick("encoder", ".onnx").ok_or_else(|| missing("encoder"))?,
        decoder: pick("decoder", ".onnx").ok_or_else(|| missing("decoder"))?,
        tokens: pick("tokens", ".txt").ok_or_else(|| missing("tokens"))?,
    })
}

/// Reads a WAV as 16 kHz mono f32, downmixing and resampling as needed.
pub fn read_wav_16k(path: &Path) -> Result<Vec<f32>> {
    let audio = |e: hound::Error| TranscribeError::Audio(format!("{}: {e}", path.display()));
    let mut reader = hound::WavReader::open(path).map_err(audio)?;
    let spec = reader.spec();
    let channels = spec.channels.max(1) as usize;
    let interleaved: Vec<f32> = match (spec.sample_format, spec.bits_per_sample) {
        (hound::SampleFormat::Int, bits) if bits <= 16 => {
            reader.samples::<i16>().map(|s| s.map(|v| v as f32 / 32768.0)).collect::<std::result::Result<_, _>>().map_err(audio)?
        }
        (hound::SampleFormat::Int, bits) => {
            let scale = (1i64 << (bits - 1)) as f32;
            reader.samples::<i32>().map(|s| s.map(|v| v as f32 / scale)).collect::<std::result::Result<_, _>>().map_err(audio)?
        }
        (hound::SampleFormat::Float, _) => reader.samples::<f32>().collect::<std::result::Result<_, _>>().map_err(audio)?,
    };
    let mono = downmix(&interleaved, channels);
    Ok(resample_linear(&mono, spec.sample_rate, SAMPLE_RATE))
}

fn downmix(samples: &[f32], channels: usize) -> Vec<f32> {
    if channels == 1 {
        return samples.to_vec();
    }
    samples.chunks(channels).map(|f| f.iter().sum::<f32>() / f.len() as f32).collect()
}

/// Linear interpolation; enough for speech going into a recognizer.
fn resample_linear(samples: &[f32], from: u32, to: u32) -> Vec<f32> {
    if from == to || samples.is_empty() {
        return samples.to_vec();
    }
    let ratio = from as f64 / to as f64;
    let out_len = (samples.len() as f64 / ratio) as usize;
    (0..out_len)
        .map(|i| {
            let pos = i as f64 * ratio;
            let idx = pos as usize;
            let frac = (pos - idx as f64) as f32;
            let a = samples[idx];
            let b = samples.get(idx + 1).copied().unwrap_or(a);
            a + (b - a) * frac
        })
        .collect()
}

/// One timed piece of the transcript.
#[derive(Debug, Clone, PartialEq)]
pub struct Cue {
    pub start_ms: u64,
    pub end_ms: u64,
    pub text: String,
}

fn timestamp(ms: u64) -> String {
    format!("{:02}:{:02}:{:02}.{:03}", ms / 3_600_000, ms / 60_000 % 60, ms / 1000 % 60, ms % 1000)
}

/// WebVTT, the format the library view renders as a timed transcript.
pub fn to_vtt(cues: &[Cue]) -> String {
    let mut out = String::from("WEBVTT\n");
    for c in cues {
        out.push_str(&format!("\n{} --> {}\n{}\n", timestamp(c.start_ms), timestamp(c.end_ms), c.text));
    }
    out
}

pub struct Transcriber {
    recognizer: WhisperRecognizer,
}

impl Transcriber {
    /// `language` is a Whisper code ("en", "hi", ...); English-only models
    /// ignore it.
    pub fn new(model_dir: &Path, language: &str) -> Result<Self> {
        let files = find_model(model_dir)?;
        let path = |p: &Path| p.to_string_lossy().to_string();
        let recognizer = WhisperRecognizer::new(WhisperConfig {
            encoder: path(&files.encoder),
            decoder: path(&files.decoder),
            tokens: path(&files.tokens),
            language: language.to_string(),
            num_threads: Some(std::thread::available_parallelism().map(|n| n.get().min(4) as i32).unwrap_or(2)),
            ..Default::default()
        })
        .map_err(|e| TranscribeError::Model(e.to_string()))?;
        Ok(Self { recognizer })
    }

    /// Transcribes a WAV into cues. `progress` gets 0.0..=1.0 after each window.
    pub fn transcribe_wav(&mut self, wav: &Path, mut progress: impl FnMut(f32)) -> Result<Vec<Cue>> {
        let samples = read_wav_16k(wav)?;
        let window = WINDOW_SECS * SAMPLE_RATE as usize;
        let total = samples.len().max(1);
        let mut cues = Vec::new();
        for (i, chunk) in samples.chunks(window).enumerate() {
            let text = self.recognizer.transcribe(SAMPLE_RATE, chunk).text.trim().to_string();
            if !text.is_empty() {
                let start = i * window;
                cues.push(Cue {
                    start_ms: start as u64 * 1000 / SAMPLE_RATE as u64,
                    end_ms: (start + chunk.len()) as u64 * 1000 / SAMPLE_RATE as u64,
                    text,
                });
            }
            progress(((i + 1) * window).min(total) as f32 / total as f32);
        }
        Ok(cues)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vtt_has_header_and_timestamps() {
        let cues = vec![
            Cue { start_ms: 0, end_ms: 28_000, text: "hello".into() },
            Cue { start_ms: 3_661_005, end_ms: 3_700_000, text: "late".into() },
        ];
        let vtt = to_vtt(&cues);
        assert!(vtt.starts_with("WEBVTT\n\n00:00:00.000 --> 00:00:28.000\nhello\n"));
        assert!(vtt.contains("01:01:01.005 --> 01:01:40.000\nlate\n"));
    }

    #[test]
    fn resample_halves_length_and_keeps_ramp() {
        let ramp: Vec<f32> = (0..32).map(|i| i as f32).collect();
        let out = resample_linear(&ramp, 32_000, 16_000);
        assert_eq!(out.len(), 16);
        assert_eq!(out[3], 6.0);
    }

    #[test]
    fn downmix_averages_channels() {
        assert_eq!(downmix(&[1.0, 3.0, 2.0, 4.0], 2), vec![2.0, 3.0]);
    }

    #[test]
    fn reads_and_converts_wav() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("a.wav");
        let spec = hound::WavSpec { channels: 2, sample_rate: 32_000, bits_per_sample: 16, sample_format: hound::SampleFormat::Int };
        let mut w = hound::WavWriter::create(&path, spec).unwrap();
        for _ in 0..3200 {
            w.write_sample(16384i16).unwrap();
            w.write_sample(-16384i16).unwrap();
        }
        w.finalize().unwrap();
        let out = read_wav_16k(&path).unwrap();
        assert_eq!(out.len(), 1600);
        assert!(out.iter().all(|s| s.abs() < 1e-6));
    }

    #[test]
    fn model_discovery_prefers_int8() {
        let dir = tempfile::tempdir().unwrap();
        for f in ["base.en-encoder.onnx", "base.en-encoder.int8.onnx", "base.en-decoder.onnx", "base.en-decoder.int8.onnx", "base.en-tokens.txt"] {
            std::fs::write(dir.path().join(f), b"").unwrap();
        }
        let m = find_model(dir.path()).unwrap();
        assert!(m.encoder.ends_with("base.en-encoder.int8.onnx"));
        assert!(m.decoder.ends_with("base.en-decoder.int8.onnx"));
        assert!(m.tokens.ends_with("base.en-tokens.txt"));
        assert!(find_model(&dir.path().join("nope")).is_err());
    }
}
