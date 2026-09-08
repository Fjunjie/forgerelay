// fr_digest_common.cpp - SHA-256 后端无关部分：hex 转换与一次性计算。
#include "forgerelay/storage/digest.hpp"

#include <string_view>

#include "forgerelay/fr_hex.h"
#include "forgerelay/storage/error.hpp"

namespace fr {

std::string sha256_to_hex(const Sha256 &digest)
{
    std::string hex(kSha256Len * 2, '\0');
    fr_hex_encode(digest.data(), kSha256Len, hex.data(), hex.size() + 1);
    return hex;
}

Sha256 sha256_from_hex(const std::string &hex)
{
    if (!fr_digest_hex_valid(hex.c_str())) {
        throw_error(FR_E_ARG, "digest hex is not 64 lowercase hex chars");
    }
    Sha256 out{};
    fr_hex_decode(hex.c_str(), out.data(), out.size(), nullptr);
    return out;
}

Sha256 sha256(const void *data, size_t len)
{
    Sha256Stream stream;
    stream.update(data, len);
    return stream.finish();
}

} // namespace fr
