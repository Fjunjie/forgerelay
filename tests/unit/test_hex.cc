// test_hex.cc - hex 编解码与摘要校验单元测试。
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/fr_hex.h"

namespace {

TEST(HexEncode, BasicAndEmpty) {
    char out[64];
    const uint8_t bytes[] = {0x01, 0xab, 0xff};
    ASSERT_EQ(FR_OK, fr_hex_encode(bytes, sizeof(bytes), out, sizeof(out)));
    EXPECT_EQ(std::string("01abff"), std::string(out));

    ASSERT_EQ(FR_OK, fr_hex_encode(nullptr, 0, out, sizeof(out)));
    EXPECT_EQ(std::string(""), std::string(out));
}

TEST(HexEncode, InsufficientCapacity) {
    char out[4];
    const uint8_t bytes[] = {0x01, 0xab, 0xff}; // 需要 7 字节。
    EXPECT_EQ(FR_E_RANGE, fr_hex_encode(bytes, sizeof(bytes), out, sizeof(out)));
    EXPECT_EQ(FR_E_ARG, fr_hex_encode(bytes, sizeof(bytes), nullptr, 16));
}

TEST(HexDecode, RoundTrip) {
    char hex[64];
    uint8_t raw[3];
    const uint8_t bytes[] = {0xde, 0xad, 0xbe};
    ASSERT_EQ(FR_OK, fr_hex_encode(bytes, sizeof(bytes), hex, sizeof(hex)));
    size_t decoded = 0;
    ASSERT_EQ(FR_OK, fr_hex_decode(hex, raw, sizeof(raw), &decoded));
    EXPECT_EQ(3u, decoded);
    EXPECT_EQ(0, std::memcmp(bytes, raw, sizeof(raw)));
}

TEST(HexDecode, AcceptsUppercase) {
    uint8_t raw[2];
    size_t decoded = 0;
    ASSERT_EQ(FR_OK, fr_hex_decode("DeAd", raw, sizeof(raw), &decoded));
    EXPECT_EQ(2u, decoded);
    EXPECT_EQ(0xde, raw[0]);
    EXPECT_EQ(0xad, raw[1]);
}

TEST(HexDecode, RejectsBadInput) {
    uint8_t raw[8];
    size_t decoded = 0;
    EXPECT_EQ(FR_E_ARG, fr_hex_decode("abc", raw, sizeof(raw), &decoded));   // 奇数长度。
    EXPECT_EQ(FR_E_ARG, fr_hex_decode("zz", raw, sizeof(raw), &decoded));    // 非法字符。
    EXPECT_EQ(FR_E_ARG, fr_hex_decode(nullptr, raw, sizeof(raw), &decoded)); // 空指针。
    EXPECT_EQ(FR_E_RANGE, fr_hex_decode("aabbcc", raw, 1, &decoded));        // 容量不足。
}

TEST(DigestHex, ValidInputs) {
    // 64 位小写 hex。
    std::string digest(64, 'a');
    EXPECT_TRUE(fr_digest_hex_valid(digest.c_str()));
    std::string mixed = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    ASSERT_EQ(64u, mixed.size());
    EXPECT_TRUE(fr_digest_hex_valid(mixed.c_str()));
}

TEST(DigestHex, InvalidInputs) {
    std::string short_digest(63, 'a');
    EXPECT_FALSE(fr_digest_hex_valid(short_digest.c_str()));
    std::string long_digest(65, 'a');
    EXPECT_FALSE(fr_digest_hex_valid(long_digest.c_str()));
    std::string upper(64, 'A');
    EXPECT_FALSE(fr_digest_hex_valid(upper.c_str()));
    std::string with_g = "g" + std::string(63, 'a');
    EXPECT_FALSE(fr_digest_hex_valid(with_g.c_str()));
    EXPECT_FALSE(fr_digest_hex_valid(nullptr));
    EXPECT_FALSE(fr_digest_hex_valid(""));
}

} // namespace
