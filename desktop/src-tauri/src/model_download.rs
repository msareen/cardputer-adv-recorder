//! Installs the default speech model (Whisper tiny.en, sherpa-onnx int8
//! export, about 103 MB) that ships inside the app bundle
//! (`resources/models/sherpa-onnx-whisper-tiny.en`, committed to this repo), so
//! nothing is fetched from a third party. It is copied to `<app data>/models`
//! the first time transcription is turned on, which keeps the path stable
//! across app updates.

use std::path::{Path, PathBuf};

pub const MODEL_NAME: &str = "sherpa-onnx-whisper-tiny.en";

/// Bundled location below the Tauri resource directory.
pub fn bundled_subdir() -> PathBuf {
    Path::new("resources").join("models").join(MODEL_NAME)
}

/// Copies the model files from `bundled` into `<models_dir>/<MODEL_NAME>`
/// (through `.part` files so an interrupted copy is never mistaken for a model)
/// and returns that folder.
pub fn install_bundled(bundled: &Path, models_dir: &Path) -> Result<PathBuf, String> {
    crate::transcribe::find_model(bundled).map_err(|_| "The bundled speech model is missing from this build.".to_string())?;
    let dest = models_dir.join(MODEL_NAME);
    std::fs::create_dir_all(&dest).map_err(|e| e.to_string())?;
    for entry in std::fs::read_dir(bundled).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if !entry.file_type().map_err(|e| e.to_string())?.is_file() {
            continue;
        }
        let name = entry.file_name();
        let target = dest.join(&name);
        let same = std::fs::metadata(&target).map(|m| m.len()).ok() == entry.metadata().map(|m| m.len()).ok();
        if same {
            continue;
        }
        let tmp = dest.join(format!("{}.part", name.to_string_lossy()));
        std::fs::copy(entry.path(), &tmp).map_err(|e| e.to_string())?;
        std::fs::rename(&tmp, &target).map_err(|e| e.to_string())?;
    }
    Ok(dest)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installs_bundled_files() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("src");
        std::fs::create_dir_all(&src).unwrap();
        for n in ["tiny.en-encoder.int8.onnx", "tiny.en-decoder.int8.onnx", "tiny.en-tokens.txt"] {
            std::fs::write(src.join(n), n).unwrap();
        }
        let out = install_bundled(&src, &dir.path().join("models")).unwrap();
        assert!(crate::transcribe::find_model(&out).is_ok());
        // second run is a no-op and still succeeds
        assert!(install_bundled(&src, &dir.path().join("models")).is_ok());
    }

    #[test]
    fn missing_bundle_is_an_error() {
        let dir = tempfile::tempdir().unwrap();
        assert!(install_bundled(&dir.path().join("none"), dir.path()).is_err());
    }

    /// The model committed to the repo is complete and loadable by `find_model`.
    #[test]
    fn repo_bundle_is_complete() {
        let p = Path::new(env!("CARGO_MANIFEST_DIR")).join(bundled_subdir());
        assert!(crate::transcribe::find_model(&p).is_ok(), "{p:?}");
    }
}
