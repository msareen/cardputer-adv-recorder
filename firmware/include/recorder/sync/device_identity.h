#pragma once

#include <cstdint>
#include <string>

namespace cardputer_recorder {

// Per-device P-256 key pair, generated on first boot and kept in NVS.
// Device id = first 16 hex chars of SHA-256 of the uncompressed public key.
// The private key never leaves this class: it is loaded from NVS for each
// ECDH or certificate operation and wiped afterwards.
class DeviceIdentity {
public:
    bool begin();
    bool ready() const { return ready_; }
    const std::string& deviceIdHex() const { return deviceIdHex_; }
    const std::uint8_t* deviceId() const { return deviceId_; }
    const std::uint8_t* publicKey() const { return publicKey_; }

    // ECDH with a peer's uncompressed public key; the x coordinate.
    bool ecdh(const std::uint8_t peerPublicKey[65], std::uint8_t shared[32]);
    // Self-signed certificate and key (PEM) for the HTTPS server.
    bool makeTlsCredentials(std::string& certificatePem,
                            std::string& privateKeyPem);

private:
    bool generate(std::uint8_t privateKey[32]);
    bool loadPrivateKey(std::uint8_t privateKey[32]);

    bool ready_ = false;
    std::uint8_t publicKey_[65] = {};
    std::uint8_t deviceId_[8] = {};
    std::string deviceIdHex_;
};

// True when `certificateDer` is an X.509 certificate for exactly
// `publicKey` (uncompressed P-256). Used to check the desktop's cert
// against the key it paired with.
bool certificateHasKey(const std::string& certificateDer,
                       const std::uint8_t publicKey[65]);
// DER to PEM ("CERTIFICATE").
bool certificateDerToPem(const std::string& der, std::string& pem);

}  // namespace cardputer_recorder
