#include <unity.h>

#include <cstring>
#include <string>

#include "recorder/sync/cbor.h"
#include "recorder/sync/pairing_math.h"
#include "recorder/sync/sha256.h"

using namespace cardputer_recorder;

namespace {

std::string hex(const std::uint8_t* data, std::size_t size)
{
    return toHex(data, size);
}

std::string hex(const std::string& data)
{
    return toHex(reinterpret_cast<const std::uint8_t*>(data.data()),
                 data.size());
}

std::string unhex(const char* text)
{
    std::string out;
    for (std::size_t i = 0; text[i] && text[i + 1]; i += 2) {
        auto nibble = [](char c) {
            return c <= '9' ? c - '0' : c - 'a' + 10;
        };
        out += static_cast<char>(nibble(text[i]) << 4 | nibble(text[i + 1]));
    }
    return out;
}

// Same fixed inputs as shared_vectors_with_firmware in
// desktop/src-tauri/src/pairing.rs.
struct Vectors {
    std::uint8_t shared[32];
    std::uint8_t hostPk[65];
    std::uint8_t devicePk[65];
    std::uint8_t hostNonce[16];
    std::uint8_t deviceNonce[16];
    Vectors()
    {
        std::memset(shared, 0x11, sizeof(shared));
        std::memset(hostPk, 0x22, sizeof(hostPk));
        hostPk[0] = 0x04;
        std::memset(devicePk, 0x33, sizeof(devicePk));
        devicePk[0] = 0x04;
        std::memset(hostNonce, 0x44, sizeof(hostNonce));
        std::memset(deviceNonce, 0x55, sizeof(deviceNonce));
    }
};

}  // namespace

void test_hmac_rfc4231_case2()
{
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    std::uint8_t out[32];
    hmacSha256(reinterpret_cast<const std::uint8_t*>(key), 4,
               reinterpret_cast<const std::uint8_t*>(data),
               std::strlen(data), out);
    TEST_ASSERT_EQUAL_STRING(
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        hex(out, 32).c_str());
}

void test_hkdf_rfc5869_case1()
{
    const std::string ikm = unhex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const std::string salt = unhex("000102030405060708090a0b0c");
    const std::string info = unhex("f0f1f2f3f4f5f6f7f8f9");
    std::uint8_t okm[42];
    hkdfSha256(reinterpret_cast<const std::uint8_t*>(salt.data()),
               salt.size(),
               reinterpret_cast<const std::uint8_t*>(ikm.data()), ikm.size(),
               reinterpret_cast<const std::uint8_t*>(info.data()),
               info.size(), okm, sizeof(okm));
    TEST_ASSERT_EQUAL_STRING(
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865",
        hex(okm, sizeof(okm)).c_str());
}

void test_vectors_shared_with_desktop()
{
    const Vectors v;
    std::uint8_t out[32];
    pairingCommitment(v.hostPk, v.hostNonce, out);
    TEST_ASSERT_EQUAL_STRING(
        "e13cafbba9ccc718f9ec5a7a3245fc5039e5d4de97f76b5551474740e8fcafee",
        hex(out, 32).c_str());
    pairingSessionKey(v.shared, v.hostPk, v.devicePk, v.hostNonce,
                      v.deviceNonce, out);
    TEST_ASSERT_EQUAL_STRING(
        "4a51c5239ed6d613897211f18e3c813c4d70a0f1e2b743716aa8fb97b686f7b8",
        hex(out, 32).c_str());
    TEST_ASSERT_EQUAL_STRING(
        "581958", pairingCode(v.hostPk, v.devicePk, v.hostNonce,
                              v.deviceNonce)
                      .c_str());
    TEST_ASSERT_EQUAL_STRING("6fb33342010e11a7", keyIdHex(v.hostPk).c_str());
}

void test_gcm_nonce_layout()
{
    std::uint8_t nonce[12];
    gcmNonce(kDirectionHost, 0x0102030405060708ULL, nonce);
    TEST_ASSERT_EQUAL_STRING("480000000102030405060708",
                             hex(nonce, 12).c_str());
}

void test_cbor_matches_desktop_bytes()
{
    CborWriter a;
    a.map(1).text("t").text("confirmed");
    TEST_ASSERT_EQUAL_STRING("a1617469636f6e6669726d6564",
                             hex(a.data()).c_str());
    CborWriter b;
    b.map(3).text("t").text("wifi").text("ok").boolean(true).text("ip").text(
        "");
    TEST_ASSERT_EQUAL_STRING("a361746477696669626f6bf562697060",
                             hex(b.data()).c_str());
}

void test_cbor_round_trip()
{
    std::uint8_t pk[65];
    std::memset(pk, 0xAB, sizeof(pk));
    CborWriter w;
    w.map(5)
        .text("t")
        .text("hello")
        .text("pk")
        .bytes(pk, sizeof(pk))
        .text("known")
        .boolean(false)
        .text("list")
        .array(2)
        .text("Home")
        .text("Guest")
        .text("n")
        .uint(300);
    CborValue v;
    TEST_ASSERT_TRUE(cborDecode(
        reinterpret_cast<const std::uint8_t*>(w.data().data()),
        w.data().size(), v));
    TEST_ASSERT_EQUAL_STRING("hello", v.text("t").c_str());
    TEST_ASSERT_EQUAL_UINT32(65, v.bytesOf("pk").size());
    TEST_ASSERT_FALSE(v.flag("known"));
    const CborValue* list = v.find("list");
    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_EQUAL_UINT32(2, list->items.size());
    TEST_ASSERT_EQUAL_STRING("Guest", list->items[1].str.c_str());
    TEST_ASSERT_TRUE(v.find("n")->uintValue == 300);
    TEST_ASSERT_TRUE(v.text("missing").empty());
}

void test_cbor_rejects_garbage()
{
    CborValue v;
    const std::uint8_t truncated[] = {0xA1, 0x61, 0x74, 0x69, 'x'};
    TEST_ASSERT_FALSE(cborDecode(truncated, sizeof(truncated), v));
    const std::uint8_t trailing[] = {0xF5, 0x00};
    TEST_ASSERT_FALSE(cborDecode(trailing, sizeof(trailing), v));
    const std::uint8_t intKey[] = {0xA1, 0x01, 0x02};
    TEST_ASSERT_FALSE(cborDecode(intKey, sizeof(intKey), v));
    const std::uint8_t hugeText[] = {0x7A, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_FALSE(cborDecode(hugeText, sizeof(hugeText), v));
    std::uint8_t deep[20];
    std::memset(deep, 0x81, sizeof(deep));  // nested one-item arrays
    TEST_ASSERT_FALSE(cborDecode(deep, sizeof(deep), v));
}

void test_fragments_reassemble()
{
    std::string message;
    for (int i = 0; i < 500; ++i) {
        message += static_cast<char>(i);
    }
    const auto fragments = fragmentMessage(message, 20);
    TEST_ASSERT_EQUAL_UINT32(27, fragments.size());
    Reassembler r;
    std::string out;
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        TEST_ASSERT_TRUE(fragments[i].size() <= 20);
        const auto result = r.push(
            reinterpret_cast<const std::uint8_t*>(fragments[i].data()),
            fragments[i].size(), out);
        TEST_ASSERT_EQUAL_INT(
            static_cast<int>(i + 1 == fragments.size()
                                 ? Reassembler::Result::kComplete
                                 : Reassembler::Result::kPending),
            static_cast<int>(result));
    }
    TEST_ASSERT_TRUE(out == message);

    const auto empty = fragmentMessage("", 20);
    TEST_ASSERT_EQUAL_UINT32(1, empty.size());
    TEST_ASSERT_EQUAL_UINT32(1, empty[0].size());

    std::string big(200, '\0');
    int errors = 0;
    for (int i = 0; i < 20; ++i) {
        if (r.push(reinterpret_cast<const std::uint8_t*>(big.data()),
                   big.size(), out) == Reassembler::Result::kError) {
            ++errors;
        }
    }
    TEST_ASSERT_TRUE(errors > 0);
}

int main(int argc, char** argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_hmac_rfc4231_case2);
    RUN_TEST(test_hkdf_rfc5869_case1);
    RUN_TEST(test_vectors_shared_with_desktop);
    RUN_TEST(test_gcm_nonce_layout);
    RUN_TEST(test_cbor_matches_desktop_bytes);
    RUN_TEST(test_cbor_round_trip);
    RUN_TEST(test_cbor_rejects_garbage);
    RUN_TEST(test_fragments_reassemble);
    return UNITY_END();
}
