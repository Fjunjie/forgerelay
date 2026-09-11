// fr_digest_common.cpp - SHA-256 后端无关部分：hex 转换、一次性计算、随机数。
#include "forgerelay/storage/digest.hpp"

#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS) (status)) >= 0)
#endif
extern "C" NTSTATUS NTAPI SystemFunction036(PVOID RandomBuffer, ULONG RandomBufferLength);
#else
#include <openssl/rand.h>
#endif

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

Sha256 random_bytes()
{
#if defined(_WIN32)
    /* 开发验证环境：CNG GenRandom（D-13）。 */
    Sha256 out{};
    const NTSTATUS rc = SystemFunction036(out.data(), static_cast<ULONG>(out.size()));
    if (!NT_SUCCESS(rc)) {
        throw_error(FR_E_INTERNAL, "SystemFunction036 (RtlGenRandom) failed");
    }
    return out;
#else
    /* 目标平台：OpenSSL RAND。 */
    Sha256 out{};
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        throw_error(FR_E_INTERNAL, "RAND_bytes failed");
    }
    return out;
#endif
}

} // namespace fr
