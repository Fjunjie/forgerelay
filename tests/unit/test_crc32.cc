// test_crc32.cc - CRC32 已知校验值测试（与 zlib/PNG 兼容）。
#include <cstring>

#include <gtest/gtest.h>

#include "forgerelay/fr_crc32.h"

namespace {

TEST(Crc32, EmptyInput) {
    EXPECT_EQ(0u, fr_crc32(nullptr, 0));
    const uint8_t dummy = 0;
    EXPECT_EQ(0u, fr_crc32(&dummy, 0));
}

TEST(Crc32, KnownCheckValue123456789) {
    // CRC-32 标准校验值："123456789" -> 0xCBF43926。
    EXPECT_EQ(0xcbf43926u, fr_crc32("123456789", 9));
}

TEST(Crc32, KnownValues) {
    EXPECT_EQ(0xe8b7be43u, fr_crc32("a", 1));
    EXPECT_EQ(0x352441c2u, fr_crc32("abc", 3));
    // 回归锁定值（算法由 0xCBF43926 标准校验值锁定）。
    EXPECT_EQ(0x20159d7fu, fr_crc32("message digest", 14));
}

TEST(Crc32, BinaryData) {
    const uint8_t data[] = {0x00, 0x01, 0x02, 0xff, 0xfe, 0xfd};
    const uint32_t crc = fr_crc32(data, sizeof(data));
    EXPECT_NE(0u, crc);
    // 确定性：同输入同输出。
    EXPECT_EQ(crc, fr_crc32(data, sizeof(data)));
    // 灵敏度：单字节差异产生不同值。
    uint8_t modified[] = {0x00, 0x01, 0x02, 0xff, 0xfe, 0xfc};
    EXPECT_NE(crc, fr_crc32(modified, sizeof(modified)));
}

} // namespace
