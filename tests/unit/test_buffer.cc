// test_buffer.cc - fr_buf 与 fr_buf_reader 单元测试。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/fr_buffer.h"

namespace {

class BufferTest : public ::testing::Test {
protected:
    void SetUp() override { fr_buf_init(&buf_); }
    void TearDown() override { fr_buf_destroy(&buf_); }
    fr_buf buf_;
};

TEST_F(BufferTest, InitStateIsEmpty) {
    EXPECT_EQ(nullptr, buf_.data);
    EXPECT_EQ(0u, buf_.len);
    EXPECT_EQ(0u, buf_.cap);
}

TEST_F(BufferTest, AppendAndConsumeRoundTrip) {
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "hello", 5));
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, " world", 6));
    EXPECT_EQ(11u, buf_.len);
    EXPECT_EQ(std::string("hello world"),
              std::string(reinterpret_cast<char *>(buf_.data), buf_.len));

    ASSERT_EQ(FR_OK, fr_buf_consume(&buf_, 6));
    EXPECT_EQ(5u, buf_.len);
    EXPECT_EQ(std::string("world"), std::string(reinterpret_cast<char *>(buf_.data), buf_.len));
}

TEST_F(BufferTest, ConsumeBeyondLengthFails) {
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "abc", 3));
    uint8_t *before = buf_.data;
    EXPECT_EQ(FR_E_RANGE, fr_buf_consume(&buf_, 4));
    EXPECT_EQ(3u, buf_.len);
    EXPECT_EQ(before, buf_.data); // 失败不修改状态。
}

TEST_F(BufferTest, AppendNullWithLengthFails) {
    EXPECT_EQ(FR_E_ARG, fr_buf_append(&buf_, nullptr, 1));
    EXPECT_EQ(0u, buf_.len);
}

TEST_F(BufferTest, AppendZeroLengthOk) {
    EXPECT_EQ(FR_OK, fr_buf_append(&buf_, nullptr, 0));
    EXPECT_EQ(0u, buf_.len);
}

TEST_F(BufferTest, ReserveOverflowDetected) {
    // SIZE_MAX 追加容量：倍增路径必然溢出，须在触碰分配器前返回（PROTO-03）。
    EXPECT_EQ(FR_E_OVERFLOW, fr_buf_reserve(&buf_, SIZE_MAX));
    EXPECT_EQ(0u, buf_.cap);
    EXPECT_EQ(nullptr, buf_.data);
    // 失败后缓冲仍可用。
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "ok", 2));
    EXPECT_EQ(2u, buf_.len);
}

TEST_F(BufferTest, TypedAppendersUseBigEndian) {
    ASSERT_EQ(FR_OK, fr_buf_append_u8(&buf_, 0x01));
    ASSERT_EQ(FR_OK, fr_buf_append_u16be(&buf_, 0x0203));
    ASSERT_EQ(FR_OK, fr_buf_append_u32be(&buf_, 0x04050607u));
    ASSERT_EQ(FR_OK, fr_buf_append_u64be(&buf_, 0x08090a0b0c0d0e0full));
    const uint8_t expect[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                              0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    ASSERT_EQ(sizeof(expect), buf_.len);
    EXPECT_EQ(0, std::memcmp(expect, buf_.data, sizeof(expect)));
}

TEST_F(BufferTest, ClearKeepsCapacity) {
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "data", 4));
    const size_t cap = buf_.cap;
    fr_buf_clear(&buf_);
    EXPECT_EQ(0u, buf_.len);
    EXPECT_EQ(cap, buf_.cap);
    // clear 后仍可继续追加。
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "x", 1));
    EXPECT_EQ(1u, buf_.len);
}

TEST_F(BufferTest, MoveTransfersOwnership) {
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "payload", 7));
    fr_buf dst;
    fr_buf_init(&dst);
    fr_buf_move(&dst, &buf_);
    EXPECT_EQ(nullptr, buf_.data);
    EXPECT_EQ(0u, buf_.len);
    ASSERT_EQ(7u, dst.len);
    EXPECT_EQ(std::string("payload"), std::string(reinterpret_cast<char *>(dst.data), dst.len));
    fr_buf_destroy(&dst);
}

TEST_F(BufferTest, DestroyIsIdempotent) {
    ASSERT_EQ(FR_OK, fr_buf_append(&buf_, "abc", 3));
    fr_buf_destroy(&buf_);
    EXPECT_EQ(nullptr, buf_.data);
    fr_buf_destroy(&buf_);
    EXPECT_EQ(nullptr, buf_.data);
}

TEST_F(BufferTest, ViewOfEmptyIsNull) {
    fr_buf_view view = fr_buf_view_of(&buf_);
    EXPECT_EQ(nullptr, view.data);
    EXPECT_EQ(0u, view.len);
}

TEST(BufferView, ReflectsContents) {
    fr_buf buf;
    fr_buf_init(&buf);
    ASSERT_EQ(FR_OK, fr_buf_append(&buf, "xyz", 3));
    fr_buf_view view = fr_buf_view_of(&buf);
    EXPECT_EQ(3u, view.len);
    EXPECT_EQ(0, std::memcmp("xyz", view.data, 3));
    fr_buf_destroy(&buf);
}

TEST_F(BufferTest, ReaderReadsSequentially) {
    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    fr_buf_reader r;
    fr_buf_reader_init(&r, data, sizeof(data));
    EXPECT_EQ(5u, fr_buf_reader_remaining(&r));

    uint8_t first = 0;
    ASSERT_EQ(FR_OK, fr_buf_reader_u8(&r, &first));
    EXPECT_EQ(0x01, first);

    uint16_t be16 = 0;
    ASSERT_EQ(FR_OK, fr_buf_reader_u16be(&r, &be16));
    EXPECT_EQ(0x0203, be16);

    EXPECT_EQ(2u, fr_buf_reader_remaining(&r));

    uint8_t tail[2] = {};
    ASSERT_EQ(FR_OK, fr_buf_reader_read(&r, tail, 2));
    EXPECT_EQ(0x04, tail[0]);
    EXPECT_EQ(0x05, tail[1]);
    EXPECT_EQ(0u, fr_buf_reader_remaining(&r));
}

TEST_F(BufferTest, ReaderOutOfBoundsKeepsPosition) {
    const uint8_t data[] = {0xaa, 0xbb};
    fr_buf_reader r;
    fr_buf_reader_init(&r, data, sizeof(data));

    uint64_t big = 0;
    EXPECT_EQ(FR_E_RANGE, fr_buf_reader_u64be(&r, &big));
    EXPECT_EQ(2u, fr_buf_reader_remaining(&r)); // 失败不前移。

    EXPECT_EQ(FR_E_RANGE, fr_buf_reader_skip(&r, 3));
    EXPECT_EQ(FR_OK, fr_buf_reader_skip(&r, 2));
    EXPECT_EQ(0u, fr_buf_reader_remaining(&r));
}

TEST_F(BufferTest, ReaderZeroLengthInit) {
    fr_buf_reader r;
    fr_buf_reader_init(&r, nullptr, 0);
    EXPECT_EQ(0u, fr_buf_reader_remaining(&r));
    uint8_t byte = 0;
    EXPECT_EQ(FR_E_RANGE, fr_buf_reader_u8(&r, &byte));
}

TEST_F(BufferTest, ReaderBytesAppendsToBuffer) {
    const uint8_t data[] = {'a', 'b', 'c', 'd'};
    fr_buf_reader r;
    fr_buf_reader_init(&r, data, sizeof(data));

    fr_buf out;
    fr_buf_init(&out);
    ASSERT_EQ(FR_OK, fr_buf_reader_bytes(&r, &out, 2));
    ASSERT_EQ(FR_OK, fr_buf_reader_bytes(&r, &out, 2));
    EXPECT_EQ(4u, out.len);
    EXPECT_EQ(std::string("abcd"), std::string(reinterpret_cast<char *>(out.data), out.len));
    EXPECT_EQ(FR_E_RANGE, fr_buf_reader_bytes(&r, &out, 1));
    fr_buf_destroy(&out);
}

TEST_F(BufferTest, ReaderWideTypes) {
    std::vector<uint8_t> data;
    auto push16 = [&data](uint16_t v) {
        data.push_back(static_cast<uint8_t>(v >> 8));
        data.push_back(static_cast<uint8_t>(v & 0xffu));
    };
    auto push32 = [&data](uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            data.push_back(static_cast<uint8_t>((v >> shift) & 0xffu));
        }
    };
    auto push64 = [&data](uint64_t v) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            data.push_back(static_cast<uint8_t>((v >> shift) & 0xffu));
        }
    };
    push16(0xbeef);
    push32(0xcafebabeu);
    push64(0x0123456789abcdefull);

    fr_buf_reader r;
    fr_buf_reader_init(&r, data.data(), data.size());
    uint16_t v16 = 0;
    uint32_t v32 = 0;
    uint64_t v64 = 0;
    ASSERT_EQ(FR_OK, fr_buf_reader_u16be(&r, &v16));
    ASSERT_EQ(FR_OK, fr_buf_reader_u32be(&r, &v32));
    ASSERT_EQ(FR_OK, fr_buf_reader_u64be(&r, &v64));
    EXPECT_EQ(0xbeef, v16);
    EXPECT_EQ(0xcafebabeu, v32);
    EXPECT_EQ(0x0123456789abcdefull, v64);
    EXPECT_EQ(0u, fr_buf_reader_remaining(&r));
}

} // namespace
