#include "recorder/sync/device_identity.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pem.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include <cstring>

#include "recorder/sync/sha256.h"

namespace cardputer_recorder {
namespace {

constexpr const char* kNamespace = "cprec_id";
constexpr const char* kPrivateKey = "priv";
constexpr const char* kPublicKey = "pub";

int hardwareRandom(void*, unsigned char* output, size_t length)
{
    esp_fill_random(output, length);
    return 0;
}

// Fills `pk` with the device key pair. Caller frees `pk`.
bool loadPk(const std::uint8_t privateKey[32], mbedtls_pk_context& pk)
{
    mbedtls_pk_init(&pk);
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) !=
        0) {
        return false;
    }
    mbedtls_ecp_keypair* pair = mbedtls_pk_ec(pk);
    return mbedtls_ecp_group_load(&pair->grp, MBEDTLS_ECP_DP_SECP256R1) ==
               0 &&
           mbedtls_mpi_read_binary(&pair->d, privateKey, 32) == 0 &&
           mbedtls_ecp_mul(&pair->grp, &pair->Q, &pair->d, &pair->grp.G,
                           hardwareRandom, nullptr) == 0;
}

}  // namespace

bool DeviceIdentity::generate(std::uint8_t privateKey[32])
{
    mbedtls_ecp_group group;
    mbedtls_mpi secret;
    mbedtls_ecp_point point;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&secret);
    mbedtls_ecp_point_init(&point);

    std::size_t written = 0;
    const bool ok =
        mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_gen_keypair(&group, &secret, &point, hardwareRandom,
                                nullptr) == 0 &&
        mbedtls_mpi_write_binary(&secret, privateKey, 32) == 0 &&
        mbedtls_ecp_point_write_binary(
            &group, &point, MBEDTLS_ECP_PF_UNCOMPRESSED, &written,
            publicKey_, sizeof(publicKey_)) == 0 &&
        written == sizeof(publicKey_);

    mbedtls_ecp_point_free(&point);
    mbedtls_mpi_free(&secret);
    mbedtls_ecp_group_free(&group);
    return ok;
}

bool DeviceIdentity::loadPrivateKey(std::uint8_t privateKey[32])
{
    Preferences preferences;
    if (!preferences.begin(kNamespace, true)) {
        return false;
    }
    const bool ok =
        preferences.getBytes(kPrivateKey, privateKey, 32) == 32;
    preferences.end();
    return ok;
}

bool DeviceIdentity::begin()
{
    ready_ = false;
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) {
        return false;
    }

    bool loaded = preferences.getBytesLength(kPublicKey) ==
                      sizeof(publicKey_) &&
                  preferences.getBytesLength(kPrivateKey) == 32 &&
                  preferences.getBytes(kPublicKey, publicKey_,
                                       sizeof(publicKey_)) ==
                      sizeof(publicKey_);
    if (!loaded) {
        std::uint8_t privateKey[32];
        if (!generate(privateKey)) {
            preferences.end();
            return false;
        }
        const bool stored =
            preferences.putBytes(kPrivateKey, privateKey, 32) == 32 &&
            preferences.putBytes(kPublicKey, publicKey_,
                                 sizeof(publicKey_)) == sizeof(publicKey_);
        std::memset(privateKey, 0, sizeof(privateKey));
        if (!stored) {
            preferences.end();
            return false;
        }
    }
    preferences.end();

    Sha256 hash;
    hash.update(publicKey_, sizeof(publicKey_));
    std::uint8_t digest[32];
    hash.finish(digest);
    std::memcpy(deviceId_, digest, sizeof(deviceId_));
    deviceIdHex_ = toHex(deviceId_, sizeof(deviceId_));
    ready_ = true;
    return true;
}

bool DeviceIdentity::ecdh(const std::uint8_t peerPublicKey[65],
                          std::uint8_t shared[32])
{
    std::uint8_t privateKey[32];
    if (!ready_ || !loadPrivateKey(privateKey)) {
        return false;
    }
    mbedtls_ecp_group group;
    mbedtls_mpi secret;
    mbedtls_mpi z;
    mbedtls_ecp_point peer;
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&secret);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&peer);

    const bool ok =
        mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_mpi_read_binary(&secret, privateKey, 32) == 0 &&
        mbedtls_ecp_point_read_binary(&group, &peer, peerPublicKey, 65) ==
            0 &&
        mbedtls_ecp_check_pubkey(&group, &peer) == 0 &&
        mbedtls_ecdh_compute_shared(&group, &z, &peer, &secret,
                                    hardwareRandom, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&z, shared, 32) == 0;

    std::memset(privateKey, 0, sizeof(privateKey));
    mbedtls_ecp_point_free(&peer);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&secret);
    mbedtls_ecp_group_free(&group);
    return ok;
}

bool DeviceIdentity::makeTlsCredentials(std::string& certificatePem,
                                        std::string& privateKeyPem)
{
    std::uint8_t privateKey[32];
    if (!ready_ || !loadPrivateKey(privateKey)) {
        return false;
    }
    mbedtls_pk_context pk;
    const bool keyOk = loadPk(privateKey, pk);
    std::memset(privateKey, 0, sizeof(privateKey));

    // PEM output is under 1 KB for a P-256 key and certificate.
    static constexpr std::size_t kBufferSize = 1200;
    unsigned char* buffer = new unsigned char[kBufferSize];
    bool ok = keyOk &&
              mbedtls_pk_write_key_pem(&pk, buffer, kBufferSize) == 0;
    if (ok) {
        privateKeyPem = reinterpret_cast<const char*>(buffer);
    }

    mbedtls_x509write_cert certificate;
    mbedtls_x509write_crt_init(&certificate);
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    const std::string name = "CN=cprec-" + deviceIdHex_;
    // Desktops pin the key, not the certificate, so dates only need to be
    // wide; the device clock may be unset when the server starts.
    ok = ok && mbedtls_mpi_lset(&serial, 1) == 0 &&
         mbedtls_x509write_crt_set_serial(&certificate, &serial) == 0 &&
         mbedtls_x509write_crt_set_validity(&certificate, "20000101000000",
                                            "20991231235959") == 0 &&
         mbedtls_x509write_crt_set_subject_name(&certificate,
                                                name.c_str()) == 0 &&
         mbedtls_x509write_crt_set_issuer_name(&certificate,
                                               name.c_str()) == 0;
    if (ok) {
        mbedtls_x509write_crt_set_version(&certificate,
                                          MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&certificate, &pk);
        mbedtls_x509write_crt_set_issuer_key(&certificate, &pk);
        ok = mbedtls_x509write_crt_pem(&certificate, buffer, kBufferSize,
                                       hardwareRandom, nullptr) == 0;
    }
    if (ok) {
        certificatePem = reinterpret_cast<const char*>(buffer);
    }

    std::memset(buffer, 0, kBufferSize);
    delete[] buffer;
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&certificate);
    mbedtls_pk_free(&pk);
    return ok;
}

bool certificateHasKey(const std::string& certificateDer,
                       const std::uint8_t publicKey[65])
{
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    bool ok = mbedtls_x509_crt_parse_der(
                  &certificate,
                  reinterpret_cast<const unsigned char*>(
                      certificateDer.data()),
                  certificateDer.size()) == 0 &&
              mbedtls_pk_get_type(&certificate.pk) == MBEDTLS_PK_ECKEY;
    if (ok) {
        const mbedtls_ecp_keypair* pair = mbedtls_pk_ec(certificate.pk);
        std::uint8_t encoded[65];
        std::size_t written = 0;
        ok = mbedtls_ecp_point_write_binary(
                 &pair->grp, &pair->Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                 &written, encoded, sizeof(encoded)) == 0 &&
             written == sizeof(encoded) &&
             std::memcmp(encoded, publicKey, sizeof(encoded)) == 0;
    }
    mbedtls_x509_crt_free(&certificate);
    return ok;
}

bool certificateDerToPem(const std::string& der, std::string& pem)
{
    std::size_t needed = 0;
    const char* header = "-----BEGIN CERTIFICATE-----\n";
    const char* footer = "-----END CERTIFICATE-----\n";
    mbedtls_pem_write_buffer(
        header, footer, reinterpret_cast<const unsigned char*>(der.data()),
        der.size(), nullptr, 0, &needed);
    if (needed == 0) {
        return false;
    }
    std::string buffer(needed, '\0');
    std::size_t written = 0;
    if (mbedtls_pem_write_buffer(
            header, footer,
            reinterpret_cast<const unsigned char*>(der.data()), der.size(),
            reinterpret_cast<unsigned char*>(&buffer[0]), buffer.size(),
            &written) != 0) {
        return false;
    }
    // `written` includes the terminating NUL.
    pem.assign(buffer.c_str());
    return true;
}

}  // namespace cardputer_recorder
