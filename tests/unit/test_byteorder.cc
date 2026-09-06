// test_byteorder.cc - 大端字节序读写单元测试（§12.1 显式字节序处理）。
#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "forgerelay/fr_byteorder.h"

namespace {

TEST(Byteorder, StoreBe16ByteLayout) {
    std::array<uint8_t, 2> raw{};
    fr_store_be16(raw.data(), 0x1234);
    EXPECT_EQ(0x12, raw[0]);
    EXPECT_EQ(0x34, raw[1]);
}

TEST(Byteorder, StoreBe32ByteLayout) {
    std::array<uint8_t, 4> raw{};
    fr_store_be32(raw.data(), 0x12345678u);
    EXPECT_EQ(0x12, raw[0]);
    EXPECT_EQ(0x34, raw[1]);
    EXPECT_EQ(0x56, raw[2]);
    EXPECT_EQ(0x78, raw[3]);
}

TEST(Byteorder, StoreBe64ByteLayout) {
    std::array<uint8_t, 8> raw{};
    fr_store_be64(raw.data(), 0x0102030405060708ull);
    for (uint8_t i = 0; i < 8; i++) {
        EXPECT_EQ(i + 1, raw[i]) << "byte " << static_cast<int>(i);
    }
}

TEST(Byteorder, LoadMatchesStore) {
    std::array<uint8_t, 8> raw{};
    fr_store_be16(raw.data(), 0xabcd);
    EXPECT_EQ(0xabcd, fr_load_be16(raw.data()));
    fr_store_be32(raw.data(), 0xdeadbeefu);
    EXPECT_EQ(0xdeadbeefu, fr_load_be32(raw.data()));
    fr_store_be64(raw.data(), 0x1122334455667788ull);
    EXPECT_EQ(0x1122334455667788ull, fr_load_be64(raw.data()));
}

TEST(Byteorder, ExtremesRoundTrip) {
    std::array<uint8_t, 8> raw{};
    const uint16_t u16s[] = {0, 1, 0x00ff, 0xff00, 0xffff};
    for (uint16_t v : u16s) {
        fr_store_be16(raw.data(), v);
        EXPECT_EQ(v, fr_load_be16(raw.data()));
    }
    const uint32_t u32s[] = {0u, 1u, 0x80000000u, 0xffffffffu};
    for (uint32_t v : u32s) {
        fr_store_be32(raw.data(), v);
        EXPECT_EQ(v, fr_load_be32(raw.data()));
    }
    const uint64_t u64s[] = {0ull, 1ull, 0x8000000000000000ull, 0xffffffffffffffffull};
    for (uint64_t v : u64s) {
        fr_store_be64(raw.data(), v);
        EXPECT_EQ(v, fr_load_be64(raw.data()));
    }
}

TEST(Byteorder, LoadFromLiteralBytes) {
    const uint8_t bytes[] = {0x00, 0x01, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(0x0001, fr_load_be16(bytes));
    EXPECT_EQ(0x00010000u, fr_load_be32(bytes));
    EXPECT_EQ(0x0001000001020304ull, fr_load_be64(bytes));
}

} // namespace
