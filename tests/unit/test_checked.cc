// test_checked.cc - 溢出检查算术单元测试（PROTO-03）。
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "forgerelay/fr_checked.h"

namespace {

TEST(CheckedAddU32, SuccessAndOverflow) {
    uint32_t out = 0;
    EXPECT_TRUE(fr_checked_add_u32(0, 0, &out));
    EXPECT_EQ(0u, out);
    EXPECT_TRUE(fr_checked_add_u32(1, 2, &out));
    EXPECT_EQ(3u, out);
    EXPECT_TRUE(fr_checked_add_u32(0xffffffffu, 0, &out));
    EXPECT_EQ(0xffffffffu, out);
    EXPECT_FALSE(fr_checked_add_u32(0xffffffffu, 1, &out));
    EXPECT_FALSE(fr_checked_add_u32(0x80000000u, 0x80000000u, &out));
}

TEST(CheckedAddU64, SuccessAndOverflow) {
    uint64_t out = 0;
    EXPECT_TRUE(fr_checked_add_u64(0x7fffffffffffffffull, 0x7fffffffffffffffull, &out));
    EXPECT_EQ(0xfffffffffffffffeull, out);
    EXPECT_FALSE(fr_checked_add_u64(0xffffffffffffffffull, 1, &out));
    EXPECT_FALSE(fr_checked_add_u64(0x8000000000000000ull, 0x8000000000000000ull, &out));
}

TEST(CheckedSubU64, SuccessAndUnderflow) {
    uint64_t out = 0;
    EXPECT_TRUE(fr_checked_sub_u64(5, 3, &out));
    EXPECT_EQ(2u, out);
    EXPECT_TRUE(fr_checked_sub_u64(0, 0, &out));
    EXPECT_EQ(0u, out);
    EXPECT_FALSE(fr_checked_sub_u64(3, 5, &out));
    EXPECT_FALSE(fr_checked_sub_u64(0, 1, &out));
}

TEST(CheckedMulU64, SuccessAndOverflow) {
    uint64_t out = 0;
    EXPECT_TRUE(fr_checked_mul_u64(0, 12345, &out));
    EXPECT_EQ(0u, out);
    EXPECT_TRUE(fr_checked_mul_u64(6, 7, &out));
    EXPECT_EQ(42u, out);
    EXPECT_TRUE(fr_checked_mul_u64(0xffffffffull, 0xffffffffull, &out));
    EXPECT_EQ(0xfffffffe00000001ull, out);
    // 2^32 * 2^32 == 2^64 超出 uint64 表示范围。
    EXPECT_FALSE(fr_checked_mul_u64(0x100000000ull, 0x100000000ull, &out));
}

TEST(CheckedMulU64, OverflowCases) {
    uint64_t out = 123;
    EXPECT_FALSE(fr_checked_mul_u64(0xffffffffffffffffull, 2, &out));
    EXPECT_EQ(123u, out); // 失败不改出参。
}

TEST(CheckedSize, AddSubMul) {
    size_t out = 0;
    EXPECT_TRUE(fr_checked_add_size(3, 4, &out));
    EXPECT_EQ(7u, out);
    EXPECT_TRUE(fr_checked_sub_size(7, 4, &out));
    EXPECT_EQ(3u, out);
    EXPECT_FALSE(fr_checked_sub_size(4, 7, &out));
    EXPECT_TRUE(fr_checked_mul_size(8, 9, &out));
    EXPECT_EQ(72u, out);
    EXPECT_FALSE(fr_checked_add_size(SIZE_MAX, 1, &out));
    EXPECT_FALSE(fr_checked_mul_size(SIZE_MAX, 2, &out));
    EXPECT_TRUE(fr_checked_mul_size(SIZE_MAX / 2, 2, &out));
    EXPECT_EQ(SIZE_MAX - (SIZE_MAX % 2), out);
}

TEST(CheckedSize, StrongGuaranteeOnFailure) {
    size_t out = 42;
    (void)fr_checked_add_size(SIZE_MAX, 1, &out);
    EXPECT_EQ(42u, out);
    (void)fr_checked_mul_size(SIZE_MAX, 2, &out);
    EXPECT_EQ(42u, out);
    (void)fr_checked_sub_size(0, 1, &out);
    EXPECT_EQ(42u, out);
}

TEST(SizeFromU64, Narrowing) {
    size_t out = 0;
    EXPECT_TRUE(fr_size_from_u64(0, &out));
    EXPECT_EQ(0u, out);
    EXPECT_TRUE(fr_size_from_u64(static_cast<uint64_t>(SIZE_MAX), &out));
    EXPECT_EQ(SIZE_MAX, out);
    if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
        uint64_t too_big = static_cast<uint64_t>(SIZE_MAX) + 1;
        EXPECT_FALSE(fr_size_from_u64(too_big, &out));
    }
}

TEST(CheckedApi, NullOutIsAccepted) {
    // out == NULL 时仅返回成败，不写入。
    EXPECT_TRUE(fr_checked_add_u64(1, 1, nullptr));
    EXPECT_FALSE(fr_checked_add_u64(std::numeric_limits<uint64_t>::max(), 1, nullptr));
    EXPECT_TRUE(fr_checked_add_size(1, 1, nullptr));
}

} // namespace
