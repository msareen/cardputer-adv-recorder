//! Mutual TLS to a paired device. The device's certificate is self-signed,
//! so instead of a CA chain the verifier accepts exactly the public key
//! learned during pairing, and still checks the handshake signature so the
//! server must hold that key. A spoofed device fails the handshake.

use std::sync::Arc;

use rustls::{
    ClientConfig, DigitallySignedStruct, SignatureScheme,
    client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier},
    crypto::{CryptoProvider, verify_tls12_signature, verify_tls13_signature},
    pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime},
};

use crate::{identity::{HostIdentity, certificate_public_key}, pairing::PublicKeyBytes};

#[derive(Debug)]
pub struct PinnedKeyVerifier {
    key: Vec<u8>,
    provider: Arc<CryptoProvider>,
}

impl PinnedKeyVerifier {
    pub fn new(key: &[u8], provider: Arc<CryptoProvider>) -> Self {
        Self { key: key.to_vec(), provider }
    }

    pub fn check(&self, cert: &CertificateDer<'_>) -> Result<(), rustls::Error> {
        match certificate_public_key(cert) {
            Some(key) if key == self.key => Ok(()),
            Some(_) => Err(rustls::Error::General("certificate is not the paired device's key".into())),
            None => Err(rustls::Error::General("unreadable certificate".into())),
        }
    }

    pub fn tls12(&self, message: &[u8], cert: &CertificateDer<'_>, dss: &DigitallySignedStruct)
    -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    pub fn tls13(&self, message: &[u8], cert: &CertificateDer<'_>, dss: &DigitallySignedStruct)
    -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.provider.signature_verification_algorithms)
    }

    pub fn schemes(&self) -> Vec<SignatureScheme> {
        self.provider.signature_verification_algorithms.supported_schemes()
    }
}

impl ServerCertVerifier for PinnedKeyVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        self.check(end_entity).map(|_| ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        self.tls12(message, cert, dss)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        self.tls13(message, cert, dss)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.schemes()
    }
}

pub fn provider() -> Arc<CryptoProvider> {
    Arc::new(rustls::crypto::ring::default_provider())
}

/// TLS settings for talking to one paired device as this host.
pub fn client_config(device_key: &PublicKeyBytes, host: &HostIdentity) -> Result<ClientConfig, String> {
    let provider = provider();
    ClientConfig::builder_with_provider(provider.clone())
        // The device's mbedtls speaks TLS 1.2.
        .with_protocol_versions(&[&rustls::version::TLS13, &rustls::version::TLS12])
        .map_err(|e| e.to_string())?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(PinnedKeyVerifier::new(device_key, provider)))
        .with_client_auth_cert(
            vec![CertificateDer::from(host.cert_der.clone())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(host.key_pkcs8_der.clone())),
        )
        .map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    //! A local TLS server stands in for the device: it presents a
    //! certificate for the device key and requires the paired host's client
    //! certificate, like the firmware's esp_https_server setup.
    use super::*;
    use crate::{
        device::{Device, HttpDevice},
        pairing::public_bytes,
    };
    use p256::SecretKey;
    use rand_core::OsRng;
    use rustls::{
        DistinguishedName, ServerConfig,
        server::danger::{ClientCertVerified, ClientCertVerifier},
    };
    use std::sync::atomic::{AtomicBool, Ordering};
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    #[derive(Debug)]
    struct PinnedClient {
        inner: PinnedKeyVerifier,
        seen: Arc<AtomicBool>,
    }

    impl ClientCertVerifier for PinnedClient {
        fn root_hint_subjects(&self) -> &[DistinguishedName] {
            &[]
        }
        fn verify_client_cert(
            &self,
            end_entity: &CertificateDer<'_>,
            _intermediates: &[CertificateDer<'_>],
            _now: UnixTime,
        ) -> Result<ClientCertVerified, rustls::Error> {
            self.inner.check(end_entity)?;
            self.seen.store(true, Ordering::SeqCst);
            Ok(ClientCertVerified::assertion())
        }
        fn verify_tls12_signature(
            &self,
            m: &[u8],
            c: &CertificateDer<'_>,
            d: &DigitallySignedStruct,
        ) -> Result<HandshakeSignatureValid, rustls::Error> {
            self.inner.tls12(m, c, d)
        }
        fn verify_tls13_signature(
            &self,
            m: &[u8],
            c: &CertificateDer<'_>,
            d: &DigitallySignedStruct,
        ) -> Result<HandshakeSignatureValid, rustls::Error> {
            self.inner.tls13(m, c, d)
        }
        fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
            self.inner.schemes()
        }
    }

    /// Serves one `/v1/info` answer per connection on 127.0.0.1, TLS 1.2
    /// only (what the device speaks).
    async fn fake_device(device: &HostIdentity, host_key: &PublicKeyBytes) -> (u16, Arc<AtomicBool>) {
        let seen = Arc::new(AtomicBool::new(false));
        let verifier = PinnedClient { inner: PinnedKeyVerifier::new(host_key, provider()), seen: seen.clone() };
        let config = ServerConfig::builder_with_provider(provider())
            .with_protocol_versions(&[&rustls::version::TLS12])
            .unwrap()
            .with_client_cert_verifier(Arc::new(verifier))
            .with_single_cert(
                vec![CertificateDer::from(device.cert_der.clone())],
                PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(device.key_pkcs8_der.clone())),
            )
            .unwrap();
        let acceptor = tokio_rustls::TlsAcceptor::from(Arc::new(config));
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let device_id = device.id.clone();
        tokio::spawn(async move {
            while let Ok((tcp, _)) = listener.accept().await {
                let acceptor = acceptor.clone();
                let device_id = device_id.clone();
                tokio::spawn(async move {
                    let Ok(mut tls) = acceptor.accept(tcp).await else { return };
                    let mut buf = vec![0u8; 2048];
                    let _ = tls.read(&mut buf).await;
                    let body = format!(
                        r#"{{"device_id":"{device_id}","firmware":"t","battery_pct":50,"free_bytes":1,"unsynced":0,"clock_set":true}}"#
                    );
                    let reply = format!(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
                        body.len()
                    );
                    let _ = tls.write_all(reply.as_bytes()).await;
                    let _ = tls.shutdown().await;
                });
            }
        });
        (port, seen)
    }

    fn identity() -> HostIdentity {
        HostIdentity::from_secret(SecretKey::random(&mut OsRng)).unwrap()
    }

    #[tokio::test]
    async fn paired_host_talks_to_its_device() {
        let device = identity();
        let host = identity();
        let (port, seen) = fake_device(&device, &host.public).await;
        let config = client_config(&device.public, &host).unwrap();
        let client = HttpDevice::paired(&format!("127.0.0.1:{port}"), config).unwrap();
        let info = client.info().await.unwrap();
        assert_eq!(info.device_id, device.id);
        assert!(seen.load(Ordering::SeqCst), "device saw the host's client certificate");
    }

    #[tokio::test]
    async fn impostor_device_is_rejected() {
        let real = identity();
        let impostor = identity();
        let host = identity();
        let (port, _) = fake_device(&impostor, &host.public).await;
        let config = client_config(&real.public, &host).unwrap();
        let client = HttpDevice::paired(&format!("127.0.0.1:{port}"), config).unwrap();
        assert!(client.info().await.is_err());
    }

    #[tokio::test]
    async fn device_rejects_an_unpaired_host() {
        let device = identity();
        let paired_host = identity();
        let stranger = identity();
        let (port, seen) = fake_device(&device, &paired_host.public).await;
        let config = client_config(&device.public, &stranger).unwrap();
        let client = HttpDevice::paired(&format!("127.0.0.1:{port}"), config).unwrap();
        assert!(client.info().await.is_err());
        assert!(!seen.load(Ordering::SeqCst));
        let _ = public_bytes(&stranger.secret);
    }
}
