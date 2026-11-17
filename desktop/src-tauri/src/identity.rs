//! Host identity: one P-256 key pair per (install, user), private key in the
//! OS keychain (Windows Credential Manager, macOS Keychain, Linux Secret
//! Service). The self-signed certificate made from it is what the device
//! pins for mutual TLS.

use p256::{SecretKey, pkcs8::EncodePrivateKey};
use rand_core::OsRng;
use sha2::{Digest, Sha256};

use crate::pairing::{PublicKeyBytes, key_id, public_bytes};

const KEYRING_SERVICE: &str = "cprec-desktop";

pub struct HostIdentity {
    pub secret: SecretKey,
    pub public: PublicKeyBytes,
    /// Same rule as device ids; the device records acks under it.
    pub id: String,
    pub cert_der: Vec<u8>,
    pub key_pkcs8_der: Vec<u8>,
}

/// Signs with RFC 6979 deterministic ECDSA, so the same key always yields
/// the same certificate bytes.
struct DeterministicKey {
    signing: p256::ecdsa::SigningKey,
    public: Vec<u8>,
}

impl rcgen::RemoteKeyPair for DeterministicKey {
    fn public_key(&self) -> &[u8] {
        &self.public
    }

    fn sign(&self, msg: &[u8]) -> Result<Vec<u8>, rcgen::Error> {
        use p256::ecdsa::{Signature, signature::Signer};
        let signature: Signature = self.signing.sign(msg);
        Ok(signature.to_der().as_bytes().to_vec())
    }

    fn algorithm(&self) -> &'static rcgen::SignatureAlgorithm {
        &rcgen::PKCS_ECDSA_P256_SHA256
    }
}

impl HostIdentity {
    pub fn from_secret(secret: SecretKey) -> Result<Self, String> {
        let public = public_bytes(&secret);
        let id = key_id(&public);
        let key_pkcs8_der = secret.to_pkcs8_der().map_err(|e| e.to_string())?.as_bytes().to_vec();
        let key_pair = rcgen::KeyPair::from_remote(Box::new(DeterministicKey {
            signing: p256::ecdsa::SigningKey::from(&secret),
            public: public.to_vec(),
        }))
        .map_err(|e| e.to_string())?;
        let mut params = rcgen::CertificateParams::new(Vec::<String>::new()).map_err(|e| e.to_string())?;
        params.distinguished_name.push(rcgen::DnType::CommonName, format!("cprec-host-{id}"));
        // The device stores this certificate at pairing and accepts only
        // those exact bytes (mbedtls trusts a self-signed client certificate
        // only by exact match). So it must come out identical on every
        // start: a serial derived from the key, fixed dates, and
        // deterministic signatures. Dates are wide because the device does
        // not check them (its clock may be unset).
        let serial = Sha256::digest([b"cprec host cert".as_slice(), public.as_slice()].concat());
        let mut serial = serial[..16].to_vec();
        serial[0] &= 0x7f; // positive INTEGER
        params.serial_number = Some(rcgen::SerialNumber::from_slice(&serial));
        params.not_before = rcgen::date_time_ymd(2000, 1, 1);
        params.not_after = rcgen::date_time_ymd(2099, 12, 31);
        let cert = params.self_signed(&key_pair).map_err(|e| e.to_string())?;
        Ok(Self { secret, public, id, cert_der: cert.der().to_vec(), key_pkcs8_der })
    }

    /// Loads this user's host key from the OS keychain, creating it on
    /// first use.
    pub fn load_or_create(user_id: i64, username: &str) -> Result<Self, String> {
        let account = format!("host-key-{user_id}-{username}");
        let entry = keyring::Entry::new(KEYRING_SERVICE, &account).map_err(keychain_error)?;
        let secret = match entry.get_password() {
            Ok(hex) => {
                let bytes = decode_hex(&hex).ok_or("Host key in the keychain is damaged.")?;
                SecretKey::from_slice(&bytes).map_err(|_| "Host key in the keychain is damaged.".to_string())?
            }
            Err(keyring::Error::NoEntry) => {
                let secret = SecretKey::random(&mut OsRng);
                let hex: String = secret.to_bytes().iter().map(|b| format!("{b:02x}")).collect();
                entry.set_password(&hex).map_err(keychain_error)?;
                secret
            }
            Err(e) => return Err(keychain_error(e)),
        };
        Self::from_secret(secret)
    }
}

fn keychain_error(e: keyring::Error) -> String {
    format!("OS keychain unavailable: {e}")
}

fn decode_hex(text: &str) -> Option<Vec<u8>> {
    if text.len() % 2 != 0 {
        return None;
    }
    (0..text.len()).step_by(2).map(|i| u8::from_str_radix(&text[i..i + 2], 16).ok()).collect()
}

/// The uncompressed EC point inside a certificate's SubjectPublicKeyInfo.
pub fn certificate_public_key(cert_der: &[u8]) -> Option<Vec<u8>> {
    let (_, cert) = x509_parser::parse_x509_certificate(cert_der).ok()?;
    Some(cert.public_key().subject_public_key.data.to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn certificate_carries_the_host_key() {
        let host = HostIdentity::from_secret(SecretKey::random(&mut OsRng)).unwrap();
        assert_eq!(certificate_public_key(&host.cert_der).unwrap(), host.public.to_vec());
        assert_eq!(host.id.len(), 16);
    }

    /// The device pins the exact bytes, so restarting the app must not
    /// change them.
    #[test]
    fn certificate_is_identical_on_every_start() {
        let secret = SecretKey::random(&mut OsRng);
        let first = HostIdentity::from_secret(secret.clone()).unwrap();
        let second = HostIdentity::from_secret(secret).unwrap();
        assert_eq!(first.cert_der, second.cert_der);
        let other = HostIdentity::from_secret(SecretKey::random(&mut OsRng)).unwrap();
        assert_ne!(first.cert_der, other.cert_der);
    }

    /// Touches the real OS keychain, so it only runs on request:
    /// `cargo test -- --ignored keychain`. Uses a throwaway entry.
    #[test]
    #[ignore]
    fn keychain_round_trip() {
        let name = format!("keychain-test-{}", std::process::id());
        let first = HostIdentity::load_or_create(-1, &name).unwrap();
        let second = HostIdentity::load_or_create(-1, &name).unwrap();
        assert_eq!(first.public, second.public, "the key persists in the keychain");
        keyring::Entry::new(KEYRING_SERVICE, &format!("host-key--1-{name}"))
            .unwrap()
            .delete_credential()
            .unwrap();
    }

    #[test]
    fn hex_round_trip() {
        assert_eq!(decode_hex("00ff10"), Some(vec![0, 255, 16]));
        assert_eq!(decode_hex("0"), None);
        assert_eq!(decode_hex("zz"), None);
    }
}
