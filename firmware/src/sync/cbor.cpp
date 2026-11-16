#include "recorder/sync/cbor.h"

#include <cstring>

namespace cardputer_recorder {
namespace {

constexpr int kMaxDepth = 6;
constexpr std::uint64_t kMaxItems = 256;

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size)
    {
    }

    bool value(CborValue& out, int depth)
    {
        if (depth > kMaxDepth || pos_ >= size_) {
            return false;
        }
        const std::uint8_t initial = data_[pos_++];
        const std::uint8_t major = initial >> 5;
        const std::uint8_t info = initial & 0x1F;
        if (major == 7) {
            if (info == 20 || info == 21) {
                out.type = CborValue::Type::kBool;
                out.boolValue = info == 21;
                return true;
            }
            if (info == 22) {
                out.type = CborValue::Type::kNull;
                return true;
            }
            return false;  // floats and undefined are not used
        }
        std::uint64_t argument = 0;
        if (!readArgument(info, argument)) {
            return false;
        }
        switch (major) {
            case 0:
                out.type = CborValue::Type::kUint;
                out.uintValue = argument;
                return true;
            case 2:
            case 3:
                if (argument > size_ - pos_) {
                    return false;
                }
                out.type = major == 2 ? CborValue::Type::kBytes
                                      : CborValue::Type::kText;
                out.str.assign(reinterpret_cast<const char*>(data_ + pos_),
                               static_cast<std::size_t>(argument));
                pos_ += static_cast<std::size_t>(argument);
                return true;
            case 4:
                if (argument > kMaxItems) {
                    return false;
                }
                out.type = CborValue::Type::kArray;
                for (std::uint64_t i = 0; i < argument; ++i) {
                    CborValue item;
                    if (!value(item, depth + 1)) {
                        return false;
                    }
                    out.items.push_back(std::move(item));
                }
                return true;
            case 5:
                if (argument > kMaxItems) {
                    return false;
                }
                out.type = CborValue::Type::kMap;
                for (std::uint64_t i = 0; i < argument; ++i) {
                    CborValue key;
                    CborValue item;
                    if (!value(key, depth + 1) ||
                        key.type != CborValue::Type::kText ||
                        !value(item, depth + 1)) {
                        return false;
                    }
                    out.entries.emplace_back(std::move(key.str),
                                             std::move(item));
                }
                return true;
            default:
                return false;  // negative ints and tags are not used
        }
    }

    bool atEnd() const { return pos_ == size_; }

private:
    bool readArgument(std::uint8_t info, std::uint64_t& argument)
    {
        if (info < 24) {
            argument = info;
            return true;
        }
        std::size_t length = 0;
        if (info == 24) {
            length = 1;
        } else if (info == 25) {
            length = 2;
        } else if (info == 26) {
            length = 4;
        } else if (info == 27) {
            length = 8;
        } else {
            return false;  // indefinite lengths are not used
        }
        if (length > size_ - pos_) {
            return false;
        }
        argument = 0;
        for (std::size_t i = 0; i < length; ++i) {
            argument = argument << 8 | data_[pos_++];
        }
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

}  // namespace

void CborWriter::head(std::uint8_t major, std::uint64_t value)
{
    const std::uint8_t m = static_cast<std::uint8_t>(major << 5);
    if (value < 24) {
        out_ += static_cast<char>(m | value);
    } else if (value <= 0xFF) {
        out_ += static_cast<char>(m | 24);
        out_ += static_cast<char>(value);
    } else if (value <= 0xFFFF) {
        out_ += static_cast<char>(m | 25);
        out_ += static_cast<char>(value >> 8);
        out_ += static_cast<char>(value);
    } else if (value <= 0xFFFFFFFFULL) {
        out_ += static_cast<char>(m | 26);
        for (int shift = 24; shift >= 0; shift -= 8) {
            out_ += static_cast<char>(value >> shift);
        }
    } else {
        out_ += static_cast<char>(m | 27);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out_ += static_cast<char>(value >> shift);
        }
    }
}

CborWriter& CborWriter::map(std::size_t entries)
{
    head(5, entries);
    return *this;
}

CborWriter& CborWriter::array(std::size_t items)
{
    head(4, items);
    return *this;
}

CborWriter& CborWriter::text(const std::string& value)
{
    head(3, value.size());
    out_ += value;
    return *this;
}

CborWriter& CborWriter::bytes(const std::uint8_t* data, std::size_t size)
{
    head(2, size);
    out_.append(reinterpret_cast<const char*>(data), size);
    return *this;
}

CborWriter& CborWriter::bytes(const std::string& data)
{
    return bytes(reinterpret_cast<const std::uint8_t*>(data.data()),
                 data.size());
}

CborWriter& CborWriter::uint(std::uint64_t value)
{
    head(0, value);
    return *this;
}

CborWriter& CborWriter::boolean(bool value)
{
    out_ += static_cast<char>(value ? 0xF5 : 0xF4);
    return *this;
}

const CborValue* CborValue::find(const char* key) const
{
    for (const auto& entry : entries) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::string CborValue::text(const char* key) const
{
    const CborValue* v = find(key);
    return v != nullptr && v->type == Type::kText ? v->str : std::string();
}

std::string CborValue::bytesOf(const char* key) const
{
    const CborValue* v = find(key);
    return v != nullptr && v->type == Type::kBytes ? v->str : std::string();
}

bool CborValue::flag(const char* key) const
{
    const CborValue* v = find(key);
    return v != nullptr && v->type == Type::kBool && v->boolValue;
}

bool cborDecode(const std::uint8_t* data, std::size_t size, CborValue& out)
{
    out = CborValue();
    if (data == nullptr || size == 0) {
        return false;
    }
    Reader reader(data, size);
    return reader.value(out, 0) && reader.atEnd();
}

}  // namespace cardputer_recorder
