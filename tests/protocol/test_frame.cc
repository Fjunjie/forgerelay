// test_frame.cc - FRLY 帧编解码协议测试（§5.2、PROTO-01…05）。
//
// 覆盖：编码字节布局、编解码往返、任意分片、多帧连续、头部截断、
// 超长负载（分配前拒绝）、未知版本/类型、非零 flags、保留 req_id、CRC 破坏。
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "forgerelay/fr_buffer.h"
#include "forgerelay/fr_byteorder.h"
#include "forgerelay/fr_crc32.h"
#include "forgerelay/fr_frame.h"

namespace {

// 手工构造帧（不依赖 fr_frame_encode），用于交叉验证编码器与解析器。
std::vector<uint8_t> build_raw_frame(uint8_t type, uint16_t flags, uint32_t req_id,
                                     const void *payload, size_t payload_len) {
    std::vector<uint8_t> frame(FR_FRAME_HEADER_SIZE + payload_len);
    std::memcpy(frame.data(), "FRLY", 4);
    frame[4] = FR_FRAME_VERSION;
    frame[5] = type;
    fr_store_be16(frame.data() + 6, flags);
    fr_store_be32(frame.data() + 8, req_id);
    fr_store_be32(frame.data() + 12, static_cast<uint32_t>(payload_len));
    fr_store_be32(frame.data() + 16, fr_crc32(frame.data(), 16));
    if (payload_len != 0) {
        std::memcpy(frame.data() + FR_FRAME_HEADER_SIZE, payload, payload_len);
    }
    return frame;
}

void expect_frame_ready(const fr_frame &frame, uint8_t type, uint32_t req_id,
                        const std::string &payload) {
    EXPECT_EQ(type, frame.type);
    EXPECT_EQ(0u, frame.flags);
    EXPECT_EQ(req_id, frame.req_id);
    EXPECT_EQ(payload.size(), frame.payload_len);
    if (payload.empty()) {
        EXPECT_EQ(nullptr, frame.payload);
    } else {
        ASSERT_NE(nullptr, frame.payload);
        EXPECT_EQ(0, std::memcmp(payload.data(), frame.payload, payload.size()));
    }
}

class FrameParserTest : public ::testing::Test {
protected:
    void SetUp() override { fr_frame_parser_init(&parser_); }
    void TearDown() override { fr_frame_parser_destroy(&parser_); }

    // 依次喂入全部数据，返回产出的帧。
    ::testing::AssertionResult feed_all(const std::vector<uint8_t> &data, fr_frame *out) {
        size_t off = 0;
        while (off < data.size()) {
            size_t consumed = 0;
            bool ready = false;
            fr_status st = fr_frame_parser_feed(&parser_, data.data() + off, data.size() - off,
                                                &consumed, out, &ready);
            if (st != FR_OK) {
                return ::testing::AssertionFailure()
                       << "feed failed at " << off << ": " << fr_status_name(st);
            }
            if (ready) {
                return ::testing::AssertionSuccess();
            }
            if (consumed == 0) {
                return ::testing::AssertionFailure() << "no progress at offset " << off;
            }
            off += consumed;
        }
        return ::testing::AssertionFailure() << "stream ended without a complete frame";
    }

    fr_frame_parser parser_;
};

TEST(MsgType, WhitelistMatchesProtocolDoc) {
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_HELLO));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_AUTH));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_PING));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_CLOSE));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_CREATE_UPLOAD));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_PUT_CHUNK));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_QUERY_UPLOAD));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_COMMIT_UPLOAD));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_ABORT_UPLOAD));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_GET_ARTIFACT));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_DATA));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_LIST_ARTIFACTS));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_SHOW_ARTIFACT));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_STATUS));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_DELETE_ARTIFACT));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_RUN_GC));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_USER_ADD));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_USER_DISABLE));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_USER_LIST));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_TOKEN_CREATE));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_TOKEN_REVOKE));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_TOKEN_LIST));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_SESSION_LIST));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_SESSION_ABORT));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_OK));
    EXPECT_TRUE(fr_msg_type_valid(FR_MSG_ERROR));

    EXPECT_FALSE(fr_msg_type_valid(0));
    EXPECT_FALSE(fr_msg_type_valid(5));
    EXPECT_FALSE(fr_msg_type_valid(15));
    EXPECT_FALSE(fr_msg_type_valid(99));
    EXPECT_FALSE(fr_msg_type_valid(150));
    EXPECT_FALSE(fr_msg_type_valid(202));
    EXPECT_FALSE(fr_msg_type_valid(255));
}

TEST(FrameEncode, HeaderByteLayout) {
    fr_buf out;
    fr_buf_init(&out);
    ASSERT_EQ(FR_OK, fr_frame_encode(&out, FR_MSG_PING, 0, 7, "abc", 3));
    ASSERT_EQ(FR_FRAME_HEADER_SIZE + 3, out.len);

    const uint8_t *raw = out.data;
    EXPECT_EQ(0, std::memcmp(raw, "FRLY", 4));
    EXPECT_EQ(FR_FRAME_VERSION, raw[4]);
    EXPECT_EQ(FR_MSG_PING, raw[5]);
    EXPECT_EQ(0u, fr_load_be16(raw + 6));                 // flags
    EXPECT_EQ(7u, fr_load_be32(raw + 8));                 // request id
    EXPECT_EQ(3u, fr_load_be32(raw + 12));                // payload length
    EXPECT_EQ(fr_crc32(raw, 16), fr_load_be32(raw + 16)); // header crc
    EXPECT_EQ(0, std::memcmp("abc", raw + 20, 3));
    fr_buf_destroy(&out);
}

TEST(FrameEncode, RejectsInvalidInputs) {
    fr_buf out;
    fr_buf_init(&out);

    EXPECT_EQ(FR_E_ARG, fr_frame_encode(nullptr, FR_MSG_PING, 0, 1, nullptr, 0));
    EXPECT_EQ(FR_E_ARG, fr_frame_encode(&out, 99, 0, 1, nullptr, 0));          // 未知类型。
    EXPECT_EQ(FR_E_ARG, fr_frame_encode(&out, FR_MSG_PING, 1, 1, nullptr, 0)); // flags 非零。
    EXPECT_EQ(FR_E_ARG, fr_frame_encode(&out, FR_MSG_PING, 0, 0, nullptr, 0)); // 保留 req_id。
    EXPECT_EQ(FR_E_ARG, fr_frame_encode(&out, FR_MSG_PING, 0, 1, nullptr, 1)); // 空负载带长度。
    EXPECT_EQ(FR_E_RANGE, fr_frame_encode(&out, FR_MSG_PUT_CHUNK, 0, 1, "x",
                                          FR_FRAME_MAX_PAYLOAD + 1)); // 超限。
    EXPECT_EQ(0u, out.len);                                           // 全部拒绝后无输出。
    fr_buf_destroy(&out);
}

TEST(FrameEncode, MaxPayloadAccepted) {
    fr_buf out;
    fr_buf_init(&out);
    std::vector<uint8_t> payload(FR_FRAME_MAX_PAYLOAD, 0x5a);
    ASSERT_EQ(FR_OK,
              fr_frame_encode(&out, FR_MSG_PUT_CHUNK, 0, 42, payload.data(), payload.size()));
    EXPECT_EQ(FR_FRAME_MAX_FRAME, out.len);
    fr_buf_destroy(&out);
}

TEST_F(FrameParserTest, RoundTripSmallFrame) {
    const std::string payload = "hello";
    fr_buf encoded;
    fr_buf_init(&encoded);
    ASSERT_EQ(FR_OK, fr_frame_encode(&encoded, FR_MSG_OK, 0, 9, payload.data(), payload.size()));

    fr_frame frame = {};
    ASSERT_TRUE(feed_all({encoded.data, encoded.data + encoded.len}, &frame));
    expect_frame_ready(frame, FR_MSG_OK, 9, payload);
    fr_buf_destroy(&encoded);
}

TEST_F(FrameParserTest, RoundTripZeroLengthPayload) {
    fr_buf encoded;
    fr_buf_init(&encoded);
    ASSERT_EQ(FR_OK, fr_frame_encode(&encoded, FR_MSG_PING, 0, 3, nullptr, 0));

    fr_frame frame = {};
    ASSERT_TRUE(feed_all({encoded.data, encoded.data + encoded.len}, &frame));
    expect_frame_ready(frame, FR_MSG_PING, 3, "");
    fr_buf_destroy(&encoded);
}

TEST_F(FrameParserTest, RoundTripLargePayload) {
    std::string payload(1024 * 1024, '\x7e'); // 1 MiB。
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<char>(i & 0xffu);
    }
    fr_buf encoded;
    fr_buf_init(&encoded);
    ASSERT_EQ(FR_OK, fr_frame_encode(&encoded, FR_MSG_DATA, 0, 11, payload.data(), payload.size()));

    fr_frame frame = {};
    ASSERT_TRUE(feed_all({encoded.data, encoded.data + encoded.len}, &frame));
    expect_frame_ready(frame, FR_MSG_DATA, 11, payload);
    fr_buf_destroy(&encoded);
}

TEST_F(FrameParserTest, ByteByByteFragmentation) {
    const std::vector<uint8_t> raw = build_raw_frame(FR_MSG_CREATE_UPLOAD, 0, 5, "body", 4);
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;

    for (size_t i = 0; i + 1 < raw.size(); i++) {
        fr_status st = fr_frame_parser_feed(&parser_, raw.data() + i, 1, &consumed, &frame, &ready);
        ASSERT_EQ(FR_OK, st) << "byte " << i;
        ASSERT_FALSE(ready) << "frame ready early at byte " << i;
        EXPECT_EQ(1u, consumed);
    }
    fr_status st =
        fr_frame_parser_feed(&parser_, raw.data() + raw.size() - 1, 1, &consumed, &frame, &ready);
    ASSERT_EQ(FR_OK, st);
    ASSERT_TRUE(ready);
    expect_frame_ready(frame, FR_MSG_CREATE_UPLOAD, 5, "body");
}

TEST_F(FrameParserTest, HeaderSplitAcrossFeeds) {
    const std::vector<uint8_t> raw = build_raw_frame(FR_MSG_STATUS, 0, 77, "P", 1);
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;

    // 前 10 字节（半个头部）。
    ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, raw.data(), 10, &consumed, &frame, &ready));
    EXPECT_FALSE(ready);
    EXPECT_EQ(10u, consumed);

    // 头部剩余 10 字节 + 负载 1 字节。
    ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, raw.data() + 10, raw.size() - 10, &consumed,
                                          &frame, &ready));
    EXPECT_TRUE(ready);
    EXPECT_EQ(raw.size() - 10, consumed);
    expect_frame_ready(frame, FR_MSG_STATUS, 77, "P");
}

TEST_F(FrameParserTest, MultipleFramesInOneFeed) {
    const std::vector<uint8_t> first = build_raw_frame(FR_MSG_PING, 0, 1, nullptr, 0);
    const std::vector<uint8_t> second = build_raw_frame(FR_MSG_OK, 0, 2, "ok!!", 4);
    std::vector<uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.end());

    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;

    ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, combined.data(), combined.size(), &consumed,
                                          &frame, &ready));
    ASSERT_TRUE(ready);
    EXPECT_EQ(first.size(), consumed);
    expect_frame_ready(frame, FR_MSG_PING, 1, "");

    // 剩余字节喂入，产出第二帧。
    ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, combined.data() + consumed,
                                          combined.size() - consumed, &consumed, &frame, &ready));
    ASSERT_TRUE(ready);
    EXPECT_EQ(second.size(), consumed);
    expect_frame_ready(frame, FR_MSG_OK, 2, "ok!!");
}

TEST_F(FrameParserTest, TruncatedStreamYieldsNoFrame) {
    const std::vector<uint8_t> raw = build_raw_frame(FR_MSG_HELLO, 0, 8, "abc", 3);
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;

    // 逐段喂入直到只剩最后 1 字节：始终不产出帧（PROTO-01 分片容忍）。
    for (size_t take = 1; take < raw.size(); take++) {
        size_t off = 0;
        while (off < take) {
            ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, raw.data() + off, take - off, &consumed,
                                                  &frame, &ready));
            ASSERT_FALSE(ready);
            ASSERT_GT(consumed, 0u);
            off += consumed;
        }
        // 重置解析器，模拟多次独立的截断场景。
        fr_frame_parser_destroy(&parser_);
        fr_frame_parser_init(&parser_);
    }
}

TEST_F(FrameParserTest, RejectsBadMagic) {
    std::vector<uint8_t> raw = build_raw_frame(FR_MSG_PING, 0, 1, nullptr, 0);
    raw[0] = 'X';
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
    EXPECT_FALSE(ready);
}

TEST_F(FrameParserTest, RejectsUnknownVersion) {
    std::vector<uint8_t> raw = build_raw_frame(FR_MSG_PING, 0, 1, nullptr, 0);
    raw[4] = FR_FRAME_VERSION + 1;
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
}

TEST_F(FrameParserTest, RejectsUnknownType) {
    std::vector<uint8_t> raw = build_raw_frame(99, 0, 1, nullptr, 0);
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
}

TEST_F(FrameParserTest, RejectsNonZeroFlags) {
    std::vector<uint8_t> raw = build_raw_frame(FR_MSG_PING, 0x0001, 1, nullptr, 0);
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
}

TEST_F(FrameParserTest, RejectsReservedRequestId) {
    std::vector<uint8_t> raw = build_raw_frame(FR_MSG_PING, 0, 0, nullptr, 0);
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
}

TEST_F(FrameParserTest, RejectsHeaderCrcMismatch) {
    std::vector<uint8_t> raw = build_raw_frame(FR_MSG_PING, 0, 1, nullptr, 0);
    raw[8] ^= 0xffu; // 破坏 req_id 字段 -> 头部 CRC 不匹配。
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
}

TEST_F(FrameParserTest, RejectsOversizedPayloadBeforeAllocation) {
    // 直接构造声明 8 MiB + 1 负载的头部；必须在分配内存前拒绝（PROTO-02）。
    std::vector<uint8_t> raw(FR_FRAME_HEADER_SIZE);
    std::memcpy(raw.data(), "FRLY", 4);
    raw[4] = FR_FRAME_VERSION;
    raw[5] = FR_MSG_PUT_CHUNK;
    fr_store_be16(raw.data() + 6, 0);
    fr_store_be32(raw.data() + 8, 1);
    fr_store_be32(raw.data() + 12, static_cast<uint32_t>(FR_FRAME_MAX_PAYLOAD) + 1);
    fr_store_be32(raw.data() + 16, fr_crc32(raw.data(), 16));

    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    EXPECT_EQ(FR_E_PROTOCOL,
              fr_frame_parser_feed(&parser_, raw.data(), raw.size(), &consumed, &frame, &ready));
    EXPECT_EQ(0u, parser_.payload_buf.len); // 未发生负载分配/写入。
}

TEST_F(FrameParserTest, PayloadValidUntilNextFeed) {
    // D-02 契约：frame.payload 指向解析器自有缓冲，下一次 feed 前保持有效。
    const std::vector<uint8_t> raw = build_raw_frame(FR_MSG_PUT_CHUNK, 0, 6, "chunk-data", 10);
    fr_frame frame = {};
    ASSERT_TRUE(feed_all(raw, &frame));

    const uint8_t *payload_ptr = frame.payload;
    ASSERT_NE(nullptr, payload_ptr);
    EXPECT_EQ(0, std::memcmp("chunk-data", payload_ptr, 10)); // 随时可读。

    // 下一次 feed（无完整帧）之后仍有效。
    size_t consumed = 0;
    bool ready = true;
    uint8_t one_byte = 'A';
    ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, &one_byte, 1, &consumed, &frame, &ready));
    EXPECT_FALSE(ready);
    EXPECT_EQ(0, std::memcmp("chunk-data", payload_ptr, 10));
}

TEST_F(FrameParserTest, SequentialFramesThroughSameParser) {
    // 同一解析器连续解析多帧（连接复用场景）。
    std::vector<uint8_t> stream;
    const std::vector<uint8_t> f1 = build_raw_frame(FR_MSG_HELLO, 0, 1, nullptr, 0);
    const std::vector<uint8_t> f2 = build_raw_frame(FR_MSG_AUTH, 0, 2, "token", 5);
    const std::vector<uint8_t> f3 = build_raw_frame(FR_MSG_ERROR, 0, 3, "err-resp", 8);
    stream.insert(stream.end(), f1.begin(), f1.end());
    stream.insert(stream.end(), f2.begin(), f2.end());
    stream.insert(stream.end(), f3.begin(), f3.end());

    fr_frame frame = {};
    const std::string payloads[] = {"", "token", "err-resp"};
    const uint8_t types[] = {FR_MSG_HELLO, FR_MSG_AUTH, FR_MSG_ERROR};
    const uint32_t ids[] = {1, 2, 3};

    size_t off = 0;
    for (int i = 0; i < 3; i++) {
        bool ready = false;
        while (off < stream.size()) {
            size_t consumed = 0;
            ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, stream.data() + off,
                                                  stream.size() - off, &consumed, &frame, &ready));
            off += consumed;
            if (ready) {
                break;
            }
        }
        ASSERT_TRUE(ready) << "frame " << i;
        expect_frame_ready(frame, types[i], ids[i], payloads[i]);
    }
    EXPECT_EQ(stream.size(), off);
}

TEST_F(FrameParserTest, ZeroLengthFeedIsAcceptable) {
    size_t consumed = 99;
    bool ready = true;
    fr_frame frame = {};
    ASSERT_EQ(FR_OK, fr_frame_parser_feed(&parser_, nullptr, 0, &consumed, &frame, &ready));
    EXPECT_FALSE(ready);
    EXPECT_EQ(0u, consumed);
}

TEST_F(FrameParserTest, RejectsNullRequiredArgs) {
    fr_frame frame = {};
    size_t consumed = 0;
    bool ready = false;
    uint8_t dummy = 0;
    EXPECT_EQ(FR_E_ARG, fr_frame_parser_feed(nullptr, &dummy, 1, &consumed, &frame, &ready));
    EXPECT_EQ(FR_E_ARG, fr_frame_parser_feed(&parser_, &dummy, 1, nullptr, &frame, &ready));
    EXPECT_EQ(FR_E_ARG, fr_frame_parser_feed(&parser_, &dummy, 1, &consumed, nullptr, &ready));
    EXPECT_EQ(FR_E_ARG, fr_frame_parser_feed(&parser_, &dummy, 1, &consumed, &frame, nullptr));
    EXPECT_EQ(FR_E_ARG, fr_frame_parser_feed(&parser_, nullptr, 1, &consumed, &frame, &ready));
}

} // namespace
