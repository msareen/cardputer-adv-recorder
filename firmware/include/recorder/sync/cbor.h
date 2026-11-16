#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cardputer_recorder {

// The CBOR subset the BLE Control messages use: unsigned ints, byte and
// text strings, arrays, maps with text keys, booleans and null. Heads use
// the shortest encoding, so output matches the desktop's ciborium byte for
// byte.
class CborWriter {
public:
    CborWriter& map(std::size_t entries);
    CborWriter& array(std::size_t items);
    CborWriter& text(const std::string& value);
    CborWriter& bytes(const std::uint8_t* data, std::size_t size);
    CborWriter& bytes(const std::string& data);
    CborWriter& uint(std::uint64_t value);
    CborWriter& boolean(bool value);
    const std::string& data() const { return out_; }

private:
    void head(std::uint8_t major, std::uint64_t value);
    std::string out_;
};

struct CborValue {
    enum class Type : std::uint8_t {
        kNone,
        kUint,
        kBytes,
        kText,
        kBool,
        kNull,
        kArray,
        kMap,
    };
    Type type = Type::kNone;
    std::uint64_t uintValue = 0;
    bool boolValue = false;
    std::string str;  // bytes or text
    std::vector<CborValue> items;
    std::vector<std::pair<std::string, CborValue>> entries;

    const CborValue* find(const char* key) const;
    // Convenience lookups on a map; empty/false when missing or mistyped.
    std::string text(const char* key) const;
    std::string bytesOf(const char* key) const;
    bool flag(const char* key) const;
};

// Decodes exactly one value spanning the whole buffer.
bool cborDecode(const std::uint8_t* data, std::size_t size, CborValue& out);

}  // namespace cardputer_recorder
