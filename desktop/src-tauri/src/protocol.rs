//! Wire types for the device's `/v1` protocol (docs/architecture). The
//! firmware (`firmware/src/sync/protocol.cpp`) and `tools/mock-device` encode
//! the same shapes.

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct DeviceInfo {
    pub device_id: String,
    pub firmware: String,
    pub battery_pct: Option<i64>,
    pub free_bytes: u64,
    pub unsynced: u64,
    pub clock_set: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct ManifestEntry {
    pub id: String,
    pub name: String,
    pub size: u64,
    pub created_ms: i64,
    pub duration_ms: u64,
    pub locked: bool,
    pub acked: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Manifest {
    pub cursor: u64,
    pub recordings: Vec<ManifestEntry>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Sha256Response {
    pub sha256: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct AckResult {
    pub acked: bool,
    pub deleted: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeleteResult {
    pub deleted: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApiErrorBody {
    pub code: String,
    pub message: String,
}

/// True for ids the device can produce: 32 lowercase hex chars. Ids are
/// used in file names, so anything else is rejected before touching disk.
pub fn is_valid_recording_id(id: &str) -> bool {
    id.len() == 32 && id.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_firmware_manifest() {
        // Exact bytes produced by protocol::encodeManifestEntry on the device.
        let json = r#"{"cursor":9,"recordings":[{"id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","name":"A \"B\".WAV","size":64044,"created_ms":0,"duration_ms":2000,"locked":true,"acked":false}]}"#;
        let m: Manifest = serde_json::from_str(json).unwrap();
        assert_eq!(m.cursor, 9);
        assert_eq!(m.recordings[0].name, "A \"B\".WAV");
        assert!(m.recordings[0].locked);
    }

    #[test]
    fn parses_info_with_unknown_battery() {
        let json = r#"{"device_id":"0123456789abcdef","firmware":"1.5.0-dev","battery_pct":null,"free_bytes":1,"unsynced":2,"clock_set":false}"#;
        let i: DeviceInfo = serde_json::from_str(json).unwrap();
        assert_eq!(i.battery_pct, None);
    }

    #[test]
    fn validates_ids() {
        assert!(is_valid_recording_id(&"a".repeat(32)));
        assert!(!is_valid_recording_id(&"A".repeat(32)));
        assert!(!is_valid_recording_id("../../etc"));
    }
}
