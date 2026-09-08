// fr_digest_bcrypt.cpp - SHA-256 后端：Windows CNG（BCrypt，D-13）。
// 仅用于开发验证环境；Linux 目标平台使用 fr_digest_ossl.cpp。
#include <windows.h>

#include <bcrypt.h>

#include <cstring>
#include <utility>
#include <vector>

#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS) (status)) >= 0)
#endif

namespace fr {

namespace {

struct BcryptState {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<uint8_t> object; // 哈希对象内存（BCryptCreateHash 复用）

    ~BcryptState()
    {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (alg != nullptr) {
            BCryptCloseAlgorithmProvider(alg, 0);
        }
    }
};

ULONG query_property(BCRYPT_ALG_HANDLE alg, LPCWSTR property)
{
    ULONG value = 0;
    ULONG bytes = 0;
    const NTSTATUS rc = BCryptGetProperty(alg, property,
                                          reinterpret_cast<PUCHAR>(&value), sizeof(value), &bytes, 0);
    if (!NT_SUCCESS(rc)) {
        throw_error(FR_E_INTERNAL, "BCryptGetProperty failed");
    }
    return value;
}

} // namespace

Sha256Stream::Sha256Stream()
{
    auto *state = new BcryptState();
    impl_ = state;
    NTSTATUS rc = BCryptOpenAlgorithmProvider(&state->alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(rc)) {
        throw_error(FR_E_INTERNAL, "BCryptOpenAlgorithmProvider(SHA256) failed");
    }
    const ULONG object_len = query_property(state->alg, BCRYPT_OBJECT_LENGTH);
    const ULONG digest_len = query_property(state->alg, BCRYPT_HASH_LENGTH);
    if (digest_len != kSha256Len) {
        throw_error(FR_E_INTERNAL, "unexpected SHA-256 digest length from CNG");
    }
    state->object.assign(object_len, 0);
    rc = BCryptCreateHash(state->alg, &state->hash, state->object.data(), object_len, nullptr, 0, 0);
    if (!NT_SUCCESS(rc)) {
        throw_error(FR_E_INTERNAL, "BCryptCreateHash failed");
    }
}

Sha256Stream::~Sha256Stream()
{
    delete static_cast<BcryptState *>(impl_);
    impl_ = nullptr;
    state_ = nullptr;
}

Sha256Stream::Sha256Stream(Sha256Stream &&other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

Sha256Stream &Sha256Stream::operator=(Sha256Stream &&other) noexcept
{
    if (this != &other) {
        delete static_cast<BcryptState *>(impl_);
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

void Sha256Stream::update(const void *data, size_t len)
{
    if (data == nullptr && len != 0) {
        throw_error(FR_E_ARG, "sha256 update: null buffer");
    }
    if (len == 0) {
        return;
    }
    auto *state = static_cast<BcryptState *>(impl_);
    /* CNG 的长度参数为 ULONG；超长输入由调用方分块（本层接口即分块语义）。 */
    const ULONG bytes = static_cast<ULONG>(len);
    if (static_cast<uint64_t>(bytes) != len) {
        throw_error(FR_E_RANGE, "sha256 update: chunk exceeds ULONG");
    }
    if (!NT_SUCCESS(BCryptHashData(state->hash, static_cast<PUCHAR>(const_cast<void *>(data)),
                                   bytes, 0))) {
        throw_error(FR_E_INTERNAL, "BCryptHashData failed");
    }
}

Sha256 Sha256Stream::finish()
{
    auto *state = static_cast<BcryptState *>(impl_);
    Sha256 out{};
    if (!NT_SUCCESS(BCryptFinishHash(state->hash, out.data(), static_cast<ULONG>(out.size()), 0))) {
        throw_error(FR_E_INTERNAL, "BCryptFinishHash failed");
    }
    return out;
}

} // namespace fr
