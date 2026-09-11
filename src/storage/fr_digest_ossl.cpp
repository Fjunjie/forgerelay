// fr_digest_ossl.cpp - SHA-256 后端：OpenSSL 3.x EVP（SEC-01，Linux 目标平台）。
// 开发验证环境（Windows）使用 fr_digest_bcrypt.cpp，本文件在 Linux 上编译。
#include <openssl/evp.h>

#include <cstring>
#include <utility>

#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"

namespace fr {

namespace {

EVP_MD_CTX *checked_ctx(void *raw)
{
    if (raw == nullptr) {
        throw_error(FR_E_STATE, "sha256 stream already moved-out");
    }
    return static_cast<EVP_MD_CTX *>(raw);
}

} // namespace

Sha256Stream::Sha256Stream() : impl_(EVP_MD_CTX_new())
{
    EVP_MD_CTX *ctx = checked_ctx(impl_);
    if (EVP_DigestInit_ex2(ctx, EVP_sha256(), nullptr) != 1) {
        throw_error(FR_E_INTERNAL, "EVP_DigestInit_ex2(SHA256) failed");
    }
}

Sha256Stream::~Sha256Stream()
{
    if (impl_ != nullptr) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(impl_));
    }
}

Sha256Stream::Sha256Stream(Sha256Stream &&other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
}

Sha256Stream &Sha256Stream::operator=(Sha256Stream &&other) noexcept
{
    if (this != &other) {
        if (impl_ != nullptr) {
            EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(impl_));
        }
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
    EVP_MD_CTX *ctx = checked_ctx(impl_);
    if (EVP_DigestUpdate(ctx, data, len) != 1) {
        throw_error(FR_E_INTERNAL, "EVP_DigestUpdate failed");
    }
}

Sha256 Sha256Stream::finish()
{
    EVP_MD_CTX *ctx = checked_ctx(impl_);
    Sha256 out{};
    size_t out_len = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &out_len) != 1 || out_len != kSha256Len) {
        throw_error(FR_E_INTERNAL, "EVP_DigestFinal_ex failed");
    }
    return out;
}

} // namespace fr
