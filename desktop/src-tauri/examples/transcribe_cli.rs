//! Transcribes a WAV with a sherpa-onnx Whisper model and prints WebVTT.
//! `cargo run --example transcribe_cli -- <model folder> <file.wav> [language]`

use std::path::Path;

use cprec_desktop_lib::transcribe::{Transcriber, to_vtt};

fn main() {
    let mut args = std::env::args().skip(1);
    let model = args.next().expect("usage: transcribe_cli <model folder> <file.wav> [language]");
    let wav = args.next().expect("usage: transcribe_cli <model folder> <file.wav> [language]");
    let language = args.next().unwrap_or_else(|| "en".to_string());
    let mut t = Transcriber::new(Path::new(&model), &language).unwrap_or_else(|e| panic!("{e}"));
    let cues = t
        .transcribe_wav(Path::new(&wav), |f| eprint!("\r{:3.0}%", f * 100.0))
        .unwrap_or_else(|e| panic!("{e}"));
    eprintln!();
    print!("{}", to_vtt(&cues));
}
