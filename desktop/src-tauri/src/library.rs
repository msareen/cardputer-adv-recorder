//! Where synced files live:
//! `<library>/<device name>/<YYYY-MM-DD>/<name>__<id first 8>.wav`,
//! so names never collide. Partial downloads go to `<library>/.partial/`.

use std::path::{Path, PathBuf};

use chrono::{Local, TimeZone};

/// Makes a string safe as one path component on Windows, macOS and Linux.
pub fn sanitize_component(raw: &str) -> String {
    let cleaned: String = raw
        .chars()
        .map(|c| match c {
            '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*' => '_',
            c if c.is_control() => '_',
            c => c,
        })
        .collect();
    let trimmed = cleaned.trim().trim_end_matches(['.', ' ']).to_string();
    let reserved = ["CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"];
    if trimmed.is_empty() || reserved.contains(&trimmed.to_uppercase().as_str()) {
        format!("_{trimmed}")
    } else {
        trimmed
    }
}

pub fn date_folder(created_ms: i64, fallback_ms: i64) -> String {
    let ms = if created_ms > 0 { created_ms } else { fallback_ms };
    Local
        .timestamp_millis_opt(ms)
        .single()
        .map(|t| t.format("%Y-%m-%d").to_string())
        .unwrap_or_else(|| "undated".to_string())
}

pub fn partial_path(library: &Path, id: &str) -> PathBuf {
    library.join(".partial").join(format!("{id}.wav.part"))
}

pub fn final_path(library: &Path, device_name: &str, name: &str, id: &str, created_ms: i64, now_ms: i64) -> PathBuf {
    let stem = name
        .rsplit_once('.')
        .filter(|(_, ext)| ext.eq_ignore_ascii_case("wav"))
        .map(|(stem, _)| stem)
        .unwrap_or(name);
    let short_id = &id[..id.len().min(8)];
    library
        .join(sanitize_component(device_name))
        .join(date_folder(created_ms, now_ms))
        .join(format!("{}__{}.wav", sanitize_component(stem), short_id))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn builds_collision_free_paths() {
        let p = final_path(Path::new("/lib"), "Cardputer ab12", "REC0001.WAV", "0123456789abcdef0123456789abcdef", 0, 0);
        let s = p.to_string_lossy().replace('\\', "/");
        assert!(s.starts_with("/lib/Cardputer ab12/"));
        assert!(s.ends_with("/REC0001__01234567.wav"));
    }

    #[test]
    fn sanitizes_hostile_names() {
        assert_eq!(sanitize_component("a/b:c"), "a_b_c");
        assert_eq!(sanitize_component(".."), "_");
        assert_eq!(sanitize_component("con"), "_con");
        assert_eq!(sanitize_component("name. "), "name");
    }
}
