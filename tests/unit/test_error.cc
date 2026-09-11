// test_error.cc - fr_status / fr_category / fr_error 单元测试（FR：ERR-01/ERR-02）。
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/fr_error.h"

// 构造枚举范围外的未知错误码以覆盖默认分支；
// 该转换会触发 GCC 的 -Wconversion（值为受控测试输入），故局部豁免。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
inline fr_status unknown_status(int value) {
    return static_cast<fr_status>(value);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

TEST(ErrorStatus, SuccessIsZero) {
    EXPECT_EQ(FR_OK, 0);
}

TEST(ErrorStatus, NamesAreStable) {
    EXPECT_STREQ("FR_OK", fr_status_name(FR_OK));
    EXPECT_STREQ("FR_E_ARG", fr_status_name(FR_E_ARG));
    EXPECT_STREQ("FR_E_IO", fr_status_name(FR_E_IO));
    EXPECT_STREQ("FR_E_PROTOCOL", fr_status_name(FR_E_PROTOCOL));
    EXPECT_STREQ("FR_E_UNKNOWN", fr_status_name(unknown_status(-99)));
}

TEST(ErrorStatus, MessagesAreNonEmpty) {
    for (int code = -18; code <= 0; code++) {
        const char *msg = fr_status_message(static_cast<fr_status>(code));
        ASSERT_NE(nullptr, msg);
        EXPECT_GT(std::strlen(msg), 0u) << "code=" << code;
    }
}

TEST(ErrorStatus, CategoryMapping) {
    EXPECT_EQ(FR_CAT_OK, fr_status_category(FR_OK));
    EXPECT_EQ(FR_CAT_ARGUMENT, fr_status_category(FR_E_ARG));
    EXPECT_EQ(FR_CAT_RANGE, fr_status_category(FR_E_RANGE));
    EXPECT_EQ(FR_CAT_RESOURCE, fr_status_category(FR_E_NOMEM));
    EXPECT_EQ(FR_CAT_RESOURCE, fr_status_category(FR_E_LIMIT));
    EXPECT_EQ(FR_CAT_IO, fr_status_category(FR_E_IO));
    EXPECT_EQ(FR_CAT_PROTOCOL, fr_status_category(FR_E_PROTOCOL));
    EXPECT_EQ(FR_CAT_PROTOCOL, fr_status_category(FR_E_TRUNCATED));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_EOF));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_STATE));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_NOTFOUND));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_EXISTS));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_CONFLICT));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_EXPIRED));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_UNAUTHENTICATED));
    EXPECT_EQ(FR_CAT_STATE, fr_status_category(FR_E_FORBIDDEN));
    EXPECT_EQ(FR_CAT_INTERNAL, fr_status_category(FR_E_INTERNAL));
    EXPECT_EQ(FR_CAT_INTERNAL, fr_status_category(FR_E_OVERFLOW));
    // 未知错误码归入内部错误类别。
    EXPECT_EQ(FR_CAT_INTERNAL, fr_status_category(unknown_status(-99)));
}

TEST(ErrorDetail, SetFormatsDetail) {
    fr_error err;
    fr_error_clear(&err);
    fr_error_set(&err, FR_E_IO, "open %s failed (attempt %d)", "chunks", 3);
    EXPECT_EQ(FR_E_IO, err.code);
    EXPECT_EQ(std::string("open chunks failed (attempt 3)"), std::string(err.detail));
}

TEST(ErrorDetail, SetTruncatesSafely) {
    fr_error err;
    std::string big(512, 'x');
    fr_error_set(&err, FR_E_RANGE, "%s", big.c_str());
    EXPECT_EQ(FR_E_RANGE, err.code);
    EXPECT_EQ(static_cast<size_t>(FR_ERROR_DETAIL_MAX - 1), std::strlen(err.detail));
    EXPECT_EQ('\0', err.detail[FR_ERROR_DETAIL_MAX - 1]);
}

TEST(ErrorDetail, SetAcceptsNullFormat) {
    fr_error err;
    fr_error_set(&err, FR_E_NOMEM, nullptr);
    EXPECT_EQ(FR_E_NOMEM, err.code);
    EXPECT_EQ('\0', err.detail[0]);
}

TEST(ErrorDetail, SetErrnoIncludesContext) {
    fr_error err;
    fr_error_set_errno(&err, FR_E_IO, ENOENT, "open read");
    EXPECT_EQ(FR_E_IO, err.code);
    const std::string detail = err.detail;
    EXPECT_NE(std::string::npos, detail.find("open read"));
    EXPECT_NE(std::string::npos, detail.find("2")); // ENOENT == 2
}

TEST(ErrorDetail, NullErrorIsNoop) {
    fr_error_set(nullptr, FR_E_IO, "ignored");
    fr_error_set_errno(nullptr, FR_E_IO, EINVAL, "ignored");
    fr_error_clear(nullptr);
    SUCCEED();
}

TEST(ErrorDetail, ClearResetsState) {
    fr_error err;
    fr_error_set(&err, FR_E_STATE, "boom");
    fr_error_clear(&err);
    EXPECT_EQ(FR_OK, err.code);
    EXPECT_EQ('\0', err.detail[0]);
}
