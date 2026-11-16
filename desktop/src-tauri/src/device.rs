//! The device as the sync engine sees it, plus the HTTP implementation.
//! The engine is generic over [`Device`] so tests can run it against an
//! in-memory fake.

use std::{future::Future, path::Path, time::Duration};

use reqwest::{Client, Response, StatusCode, header};
use tokio::io::AsyncWriteExt;

use crate::protocol::{
    AckResult, ApiErrorBody, DeleteResult, DeviceInfo, Manifest, Sha256Response,
};

#[derive(Debug, Clone, thiserror::Error, PartialEq)]
pub enum DeviceError {
    /// No response at all: wrong address, device left sync mode, WiFi down.
    #[error("device unreachable: {0}")]
    Unreachable(String),
    /// The connection broke while a body was streaming. Safe to resume.
    #[error("transfer interrupted: {0}")]
    Interrupted(String),
    /// The device answered with a `{code, message}` error.
    #[error("device error {code}: {message}")]
    Api { status: u16, code: String, message: String },
    #[error("unexpected response: {0}")]
    Protocol(String),
    #[error("disk error: {0}")]
    Io(String),
}

impl DeviceError {
    pub fn code(&self) -> Option<&str> {
        match self {
            DeviceError::Api { code, .. } => Some(code),
            _ => None,
        }
    }

    /// Errors that end the whole sync rather than one file.
    pub fn is_fatal(&self) -> bool {
        match self {
            DeviceError::Unreachable(_) | DeviceError::Io(_) => true,
            DeviceError::Api { code, .. } => {
                matches!(code.as_str(), "busy" | "low_battery" | "not_paired")
            }
            _ => false,
        }
    }
}

impl From<std::io::Error> for DeviceError {
    fn from(e: std::io::Error) -> Self {
        DeviceError::Io(e.to_string())
    }
}

pub type DeviceResult<T> = Result<T, DeviceError>;

pub trait Device: Sync {
    fn info(&self) -> impl Future<Output = DeviceResult<DeviceInfo>> + Send;
    fn set_time(&self, unix_ms: i64, tz_offset_min: i32)
    -> impl Future<Output = DeviceResult<()>> + Send;
    fn manifest(&self, since: Option<u64>) -> impl Future<Output = DeviceResult<Manifest>> + Send;
    /// Appends the recording to `part`, starting at its current length
    /// (HTTP Range), and returns the part's final length. `progress` gets
    /// the number of bytes now on disk.
    fn download(
        &self,
        id: &str,
        part: &Path,
        progress: &(dyn Fn(u64) + Send + Sync),
    ) -> impl Future<Output = DeviceResult<u64>> + Send;
    fn sha256(&self, id: &str) -> impl Future<Output = DeviceResult<String>> + Send;
    fn ack(&self, id: &str, sha256: &str) -> impl Future<Output = DeviceResult<AckResult>> + Send;
    fn delete(&self, id: &str) -> impl Future<Output = DeviceResult<bool>> + Send;
    fn done(&self) -> impl Future<Output = DeviceResult<()>> + Send;
}

/// `/v1` over plain HTTP (dev profile of milestones 2 and 3).
pub struct HttpDevice {
    base: String,
    client: Client,
}

/// Accepts `192.168.1.20`, `192.168.1.20:8080` or a full URL. Plain
/// devices (the mock, dev builds) default to http and port 8080; paired
/// devices always use https, default port 8443.
pub fn normalize_address(address: &str) -> Result<String, String> {
    normalize(address, false)
}

pub fn normalize_paired_address(address: &str) -> Result<String, String> {
    normalize(address, true)
}

fn normalize(address: &str, secure: bool) -> Result<String, String> {
    let trimmed = address.trim().trim_end_matches('/');
    if trimmed.is_empty() {
        return Err("Enter the address shown on the device.".into());
    }
    let without_scheme = trimmed
        .strip_prefix("https://")
        .or_else(|| trimmed.strip_prefix("http://"))
        .unwrap_or(trimmed);
    let scheme = if secure || trimmed.starts_with("https://") { "https" } else { "http" };
    let url = reqwest::Url::parse(&format!("{scheme}://{without_scheme}")).map_err(|e| format!("Bad address: {e}"))?;
    let host = url.host_str().ok_or("Bad address: no host")?;
    let port = url.port().unwrap_or(if scheme == "https" { 8443 } else { 8080 });
    Ok(format!("{scheme}://{host}:{port}"))
}

impl HttpDevice {
    /// Unpaired device over plain HTTP (tools/mock-device, dev builds).
    pub fn new(address: &str) -> Result<Self, String> {
        Self::build(normalize_address(address)?, Client::builder())
    }

    /// Paired device: HTTPS with mutual TLS, pinned to the device key.
    pub fn paired(address: &str, tls: rustls::ClientConfig) -> Result<Self, String> {
        Self::build(normalize_paired_address(address)?, Client::builder().use_preconfigured_tls(tls))
    }

    fn build(base: String, builder: reqwest::ClientBuilder) -> Result<Self, String> {
        let client = builder
            .connect_timeout(Duration::from_secs(5))
            // Hashing a 19 MB file on the device takes about 20 s, and the
            // device answers only after it finishes.
            .read_timeout(Duration::from_secs(90))
            .build()
            .map_err(|e| e.to_string())?;
        Ok(Self { base, client })
    }

    pub fn base(&self) -> &str {
        &self.base
    }

    fn url(&self, path: &str) -> String {
        format!("{}/v1{}", self.base, path)
    }

    async fn send(&self, req: reqwest::RequestBuilder) -> DeviceResult<Response> {
        let resp = req.send().await.map_err(|e| DeviceError::Unreachable(e.to_string()))?;
        if resp.status().is_success() {
            return Ok(resp);
        }
        let status = resp.status().as_u16();
        let text = resp.text().await.unwrap_or_default();
        Err(match serde_json::from_str::<ApiErrorBody>(&text) {
            Ok(body) => DeviceError::Api { status, code: body.code, message: body.message },
            Err(_) => DeviceError::Api { status, code: "http".into(), message: text },
        })
    }

    async fn json<T: serde::de::DeserializeOwned>(resp: Response) -> DeviceResult<T> {
        let bytes = resp.bytes().await.map_err(|e| DeviceError::Interrupted(e.to_string()))?;
        serde_json::from_slice(&bytes).map_err(|e| DeviceError::Protocol(e.to_string()))
    }
}

impl Device for HttpDevice {
    async fn info(&self) -> DeviceResult<DeviceInfo> {
        let resp = self.send(self.client.get(self.url("/info"))).await?;
        Self::json(resp).await
    }

    async fn set_time(&self, unix_ms: i64, tz_offset_min: i32) -> DeviceResult<()> {
        let body = serde_json::json!({ "unix_ms": unix_ms, "tz_offset_min": tz_offset_min });
        self.send(self.client.post(self.url("/time")).json(&body)).await?;
        Ok(())
    }

    async fn manifest(&self, since: Option<u64>) -> DeviceResult<Manifest> {
        let mut req = self.client.get(self.url("/manifest"));
        if let Some(since) = since {
            req = req.query(&[("since", since)]);
        }
        let resp = self.send(req).await?;
        Self::json(resp).await
    }

    async fn download(
        &self,
        id: &str,
        part: &Path,
        progress: &(dyn Fn(u64) + Send + Sync),
    ) -> DeviceResult<u64> {
        let offset = match tokio::fs::metadata(part).await {
            Ok(meta) => meta.len(),
            Err(_) => 0,
        };
        let mut req = self.client.get(self.url(&format!("/recordings/{id}")));
        if offset > 0 {
            req = req.header(header::RANGE, format!("bytes={offset}-"));
        }
        let mut resp = match self.send(req).await {
            // The part is already complete: the device has nothing past it.
            Err(DeviceError::Api { status: 416, .. }) if offset > 0 => return Ok(offset),
            other => other?,
        };

        let mut file = if offset > 0 && resp.status() == StatusCode::PARTIAL_CONTENT {
            tokio::fs::OpenOptions::new().append(true).open(part).await?
        } else {
            // Full body (no Range, or the device ignored it): start over.
            tokio::fs::File::create(part).await?
        };
        let mut written = if resp.status() == StatusCode::PARTIAL_CONTENT { offset } else { 0 };
        loop {
            match resp.chunk().await {
                Ok(Some(chunk)) => {
                    file.write_all(&chunk).await?;
                    written += chunk.len() as u64;
                    progress(written);
                }
                Ok(None) => break,
                Err(e) => {
                    file.flush().await?;
                    return Err(DeviceError::Interrupted(e.to_string()));
                }
            }
        }
        file.flush().await?;
        file.sync_all().await?;
        Ok(written)
    }

    async fn sha256(&self, id: &str) -> DeviceResult<String> {
        let resp = self.send(self.client.get(self.url(&format!("/recordings/{id}/sha256")))).await?;
        Ok(Self::json::<Sha256Response>(resp).await?.sha256)
    }

    async fn ack(&self, id: &str, sha256: &str) -> DeviceResult<AckResult> {
        let body = serde_json::json!({ "sha256": sha256 });
        let resp = self
            .send(self.client.post(self.url(&format!("/recordings/{id}/ack"))).json(&body))
            .await?;
        Self::json(resp).await
    }

    async fn delete(&self, id: &str) -> DeviceResult<bool> {
        let resp = self.send(self.client.delete(self.url(&format!("/recordings/{id}")))).await?;
        Ok(Self::json::<DeleteResult>(resp).await?.deleted)
    }

    async fn done(&self) -> DeviceResult<()> {
        self.send(self.client.post(self.url("/done"))).await?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_addresses() {
        assert_eq!(normalize_address("192.168.1.20").unwrap(), "http://192.168.1.20:8080");
        assert_eq!(normalize_address(" 10.0.0.5:9000/ ").unwrap(), "http://10.0.0.5:9000");
        assert_eq!(normalize_address("http://127.0.0.1:8080").unwrap(), "http://127.0.0.1:8080");
        assert!(normalize_address("").is_err());
        assert_eq!(normalize_paired_address("192.168.1.20").unwrap(), "https://192.168.1.20:8443");
        assert_eq!(normalize_paired_address("http://10.0.0.5:9000").unwrap(), "https://10.0.0.5:9000");
    }

    #[test]
    fn classifies_fatal_errors() {
        let busy = DeviceError::Api { status: 409, code: "busy".into(), message: String::new() };
        let missing = DeviceError::Api { status: 404, code: "not_found".into(), message: String::new() };
        assert!(busy.is_fatal());
        assert!(!missing.is_fatal());
        assert!(DeviceError::Unreachable("x".into()).is_fatal());
        assert!(!DeviceError::Interrupted("x".into()).is_fatal());
    }
}
