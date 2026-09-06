// test_path.cc - 标识校验与路径映射单元测试（FR-ART-02/03、FR-STO-02、SEC-05）。
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/fr_path.h"

namespace {

std::string repeat(char c, size_t n) {
    return std::string(n, c);
}

TEST(IdentComponent, ValidValues) {
    EXPECT_TRUE(fr_ident_valid_component("a"));
    EXPECT_TRUE(fr_ident_valid_component("build-artifacts"));
    EXPECT_TRUE(fr_ident_valid_component("com.example"));
    EXPECT_TRUE(fr_ident_valid_component("v1_2"));
    EXPECT_TRUE(fr_ident_valid_component(repeat('a', 64).c_str())); // 上边界。
}

TEST(IdentComponent, InvalidValues) {
    EXPECT_FALSE(fr_ident_valid_component(nullptr));
    EXPECT_FALSE(fr_ident_valid_component(""));
    EXPECT_FALSE(fr_ident_valid_component(repeat('a', 65).c_str())); // 超长。
    EXPECT_FALSE(fr_ident_valid_component("a/b"));                   // 路径分隔符。
    EXPECT_FALSE(fr_ident_valid_component("a b"));                   // 空格。
    EXPECT_FALSE(fr_ident_valid_component("a:b"));                   // 冒号。
    EXPECT_FALSE(fr_ident_valid_component("示例"));                  // 非 ASCII。
    EXPECT_FALSE(fr_ident_valid_component("a\tb"));                  // 控制字符。
    EXPECT_FALSE(fr_ident_valid_component("."));                     // 点段。
    EXPECT_FALSE(fr_ident_valid_component(".."));                    // 父段。
}

TEST(IdentComponent, HyphenAndUnderscoreLeadingAllowed) {
    // FR-ART-02 字符集为 allowlist，未禁止首字符为连字符/下划线。
    EXPECT_TRUE(fr_ident_valid_component("-a"));
    EXPECT_TRUE(fr_ident_valid_component("_a"));
}

TEST(IdentVersion, ValidValues) {
    EXPECT_TRUE(fr_ident_valid_version("1.2.3"));
    EXPECT_TRUE(fr_ident_valid_version("1.2.3-beta.1+build.5"));
    EXPECT_TRUE(fr_ident_valid_version("2026-09-07"));
    EXPECT_TRUE(fr_ident_valid_version("9f2c1a7b8d4e"));
    EXPECT_TRUE(fr_ident_valid_version(repeat('x', 96).c_str())); // 上边界。
    EXPECT_TRUE(fr_ident_valid_version("1.0.0+20130313144700"));
}

TEST(IdentVersion, InvalidValues) {
    EXPECT_FALSE(fr_ident_valid_version(nullptr));
    EXPECT_FALSE(fr_ident_valid_version(""));
    EXPECT_FALSE(fr_ident_valid_version(repeat('x', 97).c_str()));
    EXPECT_FALSE(fr_ident_valid_version("1.2.3 beta"));
    EXPECT_FALSE(fr_ident_valid_version("1.2.3/4"));
    EXPECT_FALSE(fr_ident_valid_version("版本1"));
    EXPECT_FALSE(fr_ident_valid_version(".."));
}

TEST(ChunkRelPath, MapsDigestToTwoLevelPath) {
    const std::string digest = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    char out[FR_CHUNK_REL_PATH_LEN];
    ASSERT_EQ(FR_OK, fr_chunk_rel_path(digest.c_str(), out, sizeof(out)));
    EXPECT_EQ(std::string("e3/b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
              std::string(out));
}

TEST(ChunkRelPath, RejectsBadDigest) {
    char out[FR_CHUNK_REL_PATH_LEN];
    EXPECT_EQ(FR_E_ARG, fr_chunk_rel_path(nullptr, out, sizeof(out)));
    EXPECT_EQ(FR_E_ARG, fr_chunk_rel_path("XYZ", out, sizeof(out)));
    EXPECT_EQ(FR_E_RANGE, fr_chunk_rel_path(repeat('a', 64).c_str(), out, 4)); // 容量不足。
}

TEST(ValidateRel, AcceptsWellFormedPaths) {
    EXPECT_EQ(FR_OK, fr_path_validate_rel("file.bin"));
    EXPECT_EQ(FR_OK, fr_path_validate_rel("ab/cdef"));
    EXPECT_EQ(FR_OK, fr_path_validate_rel("a/b/c.tar.gz"));
    EXPECT_EQ(FR_OK, fr_path_validate_rel("x-y_z.0"));
}

TEST(ValidateRel, RejectsTraversalAndOddities) {
    EXPECT_EQ(FR_E_ARG, fr_path_validate_rel(nullptr));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel(""));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("/absolute")); // 绝对路径（SEC-05）。
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a/.."));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("../a"));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a/../b"));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a/./b"));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("."));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel(".."));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a//b")); // 空分量。
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a/"));   // 末尾分隔符。
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a\\b")); // 反斜杠。
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a b"));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a:b"));
    EXPECT_EQ(FR_E_RANGE, fr_path_validate_rel("a\nb")); // 控制字符。
}

TEST(PathJoin, ConcatenatesRootAndRel) {
    char out[FR_PATH_BUF_MAX];
    ASSERT_EQ(FR_OK, fr_path_join("/var/lib/forgerelay", "ab/cdef", out, sizeof(out)));
    EXPECT_EQ(std::string("/var/lib/forgerelay/ab/cdef"), std::string(out));
}

TEST(PathJoin, ToleratesTrailingSlashInRoot) {
    char out[FR_PATH_BUF_MAX];
    ASSERT_EQ(FR_OK, fr_path_join("/var/lib/", "chunks/x", out, sizeof(out)));
    EXPECT_EQ(std::string("/var/lib/chunks/x"), std::string(out));
    ASSERT_EQ(FR_OK, fr_path_join("/", "f", out, sizeof(out)));
    EXPECT_EQ(std::string("/f"), std::string(out));
}

TEST(PathJoin, RejectsBadInput) {
    char out[FR_PATH_BUF_MAX];
    EXPECT_EQ(FR_E_ARG, fr_path_join(nullptr, "x", out, sizeof(out)));
    EXPECT_EQ(FR_E_ARG, fr_path_join("/root", nullptr, out, sizeof(out)));
    EXPECT_EQ(FR_E_RANGE, fr_path_join("", "x", out, sizeof(out)));
    EXPECT_EQ(FR_E_RANGE, fr_path_join("/root", "../escape", out, sizeof(out)));
    EXPECT_EQ(FR_E_RANGE, fr_path_join("/root", "/abs", out, sizeof(out)));
    EXPECT_EQ(FR_E_RANGE, fr_path_join("/root", "x", out, 4)); // 容量不足。
}

} // namespace
