// test_digest.cc - SHA-256 摘要单元测试（已知向量；后端：BCrypt/OpenSSL）。
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/storage/digest.hpp"
#include "forgerelay/storage/error.hpp"

namespace {

std::string hex_of(const std::string &input) {
    return fr::sha256_to_hex(fr::sha256(input.data(), input.size()));
}

TEST(Digest, KnownVectors) {
    // SHA-256 标准测试向量（NIST FIPS 180-4）。
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex_of(""));
    EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex_of("abc"));
    EXPECT_EQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
}

TEST(Digest, StreamingMatchesOneShot) {
    std::vector<uint8_t> data(1024 * 1024);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>(i * 31u + 7u);
    }
    const std::string one_shot = fr::sha256_to_hex(fr::sha256(data.data(), data.size()));

    fr::Sha256Stream stream;
    size_t offset = 0;
    while (offset < data.size()) {
        const size_t take = std::min<size_t>(4096 + offset * 3 % 65536, data.size() - offset);
        stream.update(data.data() + offset, take);
        offset += take;
    }
    EXPECT_EQ(one_shot, fr::sha256_to_hex(stream.finish()));
}

TEST(Digest, StreamingEmpty) {
    fr::Sha256Stream stream;
    EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              fr::sha256_to_hex(stream.finish()));
}

TEST(Digest, HexRoundTripAndValidation) {
    const fr::Sha256 digest = fr::sha256("forgerelay", 10);
    const std::string hex = fr::sha256_to_hex(digest);
    EXPECT_EQ(64u, hex.size());
    const fr::Sha256 parsed = fr::sha256_from_hex(hex);
    EXPECT_EQ(digest, parsed);

    EXPECT_THROW(fr::sha256_from_hex("XYZ"), fr::Error);
    std::string short_hex(63, 'a');
    try {
        (void)fr::sha256_from_hex(short_hex);
        FAIL() << "expected fr::Error";
    } catch (const fr::Error &err) {
        EXPECT_EQ(FR_E_ARG, err.code());
    }
}

} // namespace
