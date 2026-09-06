// test_fd.cc - fd / 文件系统原语单元测试（FR-DL-02、FR-STO-04、LIFE-02、SEC-03）。
//
// 注：POSIX 分支的运行级验证（fsync 持久性、rename 原子性）需在 Linux 目标环境复核
// （DECISIONS D-08）；本文件在 Windows 开发分支上完整运行，POSIX 路径待 M2 前置验证。
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "forgerelay/fr_buffer.h"
#include "forgerelay/fr_fd.h"
#include "forgerelay/fr_version.h"

namespace {

class FdTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::string(::testing::TempDir()) + "fr_fd_test";
        ASSERT_EQ(FR_OK, fr_dir_create_all(root_.c_str(), &err_));
    }

    void TearDown() override {
        if (fd_ != FR_FD_INVALID) {
            (void)fr_fd_close(&fd_, nullptr);
        }
        // 递归删除测试目录内容（尽力而为，失败不影响测试结果）。
#ifdef _WIN32
        std::string cmd = "rmdir /s /q \"" + root_ + "\" >nul 2>&1";
#else
        std::string cmd = "rm -rf \"" + root_ + "\" 2>/dev/null";
#endif
        (void)std::system(cmd.c_str()); // 测试代码允许；生产代码禁止（SEC-06）。
    }

    std::string path(const std::string &name) const { return root_ + "/" + name; }

    std::string root_;
    fr_fd fd_ = FR_FD_INVALID;
    fr_error err_;
};

TEST_F(FdTest, VersionSmoke) {
    // M0 基础冒烟：核心库版本字符串与协议版本可用。
    EXPECT_STREQ("libfrcore 0.1.0", fr_core_version_string());
    EXPECT_EQ(1, FR_PROTOCOL_VERSION);
}

TEST_F(FdTest, WriteReadRoundTrip) {
    const std::string file = path("roundtrip.bin");
    const char payload[] = "forgerelay artifact data \x01\x02\xff";
    const size_t payload_len = sizeof(payload) - 1; // 含不可打印字节，不含 NUL。

    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, payload, payload_len, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    uint64_t size = 0;
    ASSERT_EQ(FR_OK, fr_file_size(file.c_str(), &size, &err_));
    EXPECT_EQ(payload_len, size);

    ASSERT_EQ(FR_OK, fr_fd_open_read(file.c_str(), &fd_, &err_));
    char readback[64] = {};
    ASSERT_EQ(FR_OK, fr_fd_read_full(fd_, readback, payload_len, &err_));
    EXPECT_EQ(0, std::memcmp(payload, readback, payload_len));
    // 精确读满后再次读取应为 EOF。
    size_t nread = 1;
    ASSERT_EQ(FR_OK, fr_fd_read(fd_, readback, 1, &nread, &err_));
    EXPECT_EQ(0u, nread);
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, SeekAndPartialRead) {
    const std::string file = path("seek.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "hello world", 11, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    ASSERT_EQ(FR_OK, fr_fd_open_read(file.c_str(), &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_seek_set(fd_, 6, &err_));
    char buf[6] = {};
    size_t nread = 0;
    ASSERT_EQ(FR_OK, fr_fd_read(fd_, buf, 5, &nread, &err_));
    EXPECT_EQ(5u, nread);
    EXPECT_EQ(std::string("world"), std::string(buf, nread));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, ReadFullDetectsEof) {
    const std::string file = path("short.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "abc", 3, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    ASSERT_EQ(FR_OK, fr_fd_open_read(file.c_str(), &fd_, &err_));
    char buf[8] = {};
    EXPECT_EQ(FR_E_EOF, fr_fd_read_full(fd_, buf, 8, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, OpenMissingFileIsNotFound) {
    fr_fd fd = FR_FD_INVALID;
    EXPECT_EQ(FR_E_NOTFOUND, fr_fd_open_read(path("no_such_file.bin").c_str(), &fd, &err_));
    EXPECT_EQ(FR_FD_INVALID, fd);
}

TEST_F(FdTest, ExclusiveCreateConflicts) {
    const std::string file = path("exclusive.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), true, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    fr_fd second = FR_FD_INVALID;
    EXPECT_EQ(FR_E_EXISTS, fr_fd_open_write(file.c_str(), true, &second, &err_));
    EXPECT_EQ(FR_FD_INVALID, second);
    // 非 exclusive 仍可打开（截断语义）。
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, CloseIsIdempotent) {
    const std::string file = path("close.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
    EXPECT_EQ(FR_FD_INVALID, fd_);
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_)); // 重复关闭无副作用。
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, FsyncSucceeds) {
    const std::string file = path("fsync.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "x", 1, &err_));
    ASSERT_EQ(FR_OK, fr_fd_fsync(fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
    SUCCEED();
}

TEST_F(FdTest, RenameReplacesAtomically) {
    const std::string a = path("a.txt");
    const std::string b = path("b.txt");

    ASSERT_EQ(FR_OK, fr_fd_open_write(a.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "content-a", 9, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    ASSERT_EQ(FR_OK, fr_fd_open_write(b.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "content-b-longer", 16, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    // rename a -> b：b 被原子替换为 a 的内容（FR-STO-04 语义）。
    ASSERT_EQ(FR_OK, fr_file_rename(a.c_str(), b.c_str(), &err_));

    bool exists = true;
    ASSERT_EQ(FR_OK, fr_file_exists(a.c_str(), &exists, &err_));
    EXPECT_FALSE(exists);

    ASSERT_EQ(FR_OK, fr_fd_open_read(b.c_str(), &fd_, &err_));
    char buf[32] = {};
    ASSERT_EQ(FR_OK, fr_fd_read_full(fd_, buf, 9, &err_));
    EXPECT_EQ(std::string("content-a"), std::string(buf, 9));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, RenameMissingSourceFails) {
    EXPECT_EQ(FR_E_NOTFOUND,
              fr_file_rename(path("missing.src").c_str(), path("missing.dst").c_str(), &err_));
}

TEST_F(FdTest, UnlinkAndIdempotency) {
    const std::string file = path("unlink-me.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "x", 1, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    ASSERT_EQ(FR_OK, fr_file_unlink(file.c_str(), &err_));
    bool exists = false;
    ASSERT_EQ(FR_OK, fr_file_exists(file.c_str(), &exists, &err_));
    EXPECT_FALSE(exists);
    EXPECT_EQ(FR_E_NOTFOUND, fr_file_unlink(file.c_str(), &err_)); // 二次删除报不存在。
}

TEST_F(FdTest, DirCreateIsIdempotent) {
    const std::string dir = path("onedir");
    EXPECT_EQ(FR_OK, fr_dir_create(dir.c_str(), &err_));
    EXPECT_EQ(FR_OK, fr_dir_create(dir.c_str(), &err_));
}

TEST_F(FdTest, DirCreateAllNested) {
    const std::string dir = path("l1/l2/l3");
    ASSERT_EQ(FR_OK, fr_dir_create_all(dir.c_str(), &err_));
    // 已存在时幂等。
    ASSERT_EQ(FR_OK, fr_dir_create_all(dir.c_str(), &err_));
    // 该目录可用于创建文件。
    ASSERT_EQ(FR_OK, fr_fd_open_write((dir + "/leaf.txt").c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, DirCreateFailsWhenFileExists) {
    const std::string file = path("occupier");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
    EXPECT_EQ(FR_E_EXISTS, fr_dir_create(file.c_str(), &err_));
}

TEST_F(FdTest, DirSyncOk) {
    ASSERT_EQ(FR_OK, fr_dir_sync(root_.c_str(), &err_));
    EXPECT_EQ(FR_E_NOTFOUND, fr_dir_sync(path("no-such-dir").c_str(), &err_));
}

TEST_F(FdTest, TmpfileCreateExclusive) {
    char name[FR_TMPNAME_MAX];
    ASSERT_EQ(FR_OK, fr_tmpfile_create(root_.c_str(), "chunk", name, sizeof(name), &fd_, &err_));
    EXPECT_EQ(0, std::strncmp(name, "frtmp-chunk-", 12));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "tmp-bytes", 9, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    bool exists = false;
    ASSERT_EQ(FR_OK, fr_file_exists(path(name).c_str(), &exists, &err_));
    EXPECT_TRUE(exists);

    // 再次创建：得到不同名称（O_EXCL 不冲突）。
    char name2[FR_TMPNAME_MAX];
    ASSERT_EQ(FR_OK, fr_tmpfile_create(root_.c_str(), "chunk", name2, sizeof(name2), &fd_, &err_));
    EXPECT_STRNE(name, name2);
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, TmpfileRejectsBadInput) {
    char name[FR_TMPNAME_MAX];
    EXPECT_EQ(FR_E_ARG,
              fr_tmpfile_create(root_.c_str(), "bad prefix!", name, sizeof(name), &fd_, &err_));
    EXPECT_EQ(FR_E_ARG, fr_tmpfile_create("", "x", name, sizeof(name), &fd_, &err_));
    char small[8];
    EXPECT_EQ(FR_E_RANGE, fr_tmpfile_create(root_.c_str(), "x", small, sizeof(small), &fd_, &err_));
    // 默认前缀。
    ASSERT_EQ(FR_OK, fr_tmpfile_create(root_.c_str(), nullptr, name, sizeof(name), &fd_, &err_));
    EXPECT_EQ(0, std::strncmp(name, "frtmp-t-", 8));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));
}

TEST_F(FdTest, ReadLimitedHonorsCap) {
    const std::string file = path("limited.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_write_all(fd_, "abcdef", 6, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    fr_buf out;
    fr_buf_init(&out);
    ASSERT_EQ(FR_OK, fr_file_read_limited(file.c_str(), &out, 64, &err_));
    EXPECT_EQ(6u, out.len);
    EXPECT_EQ(std::string("abcdef"), std::string(reinterpret_cast<char *>(out.data), out.len));

    fr_buf_clear(&out);
    EXPECT_EQ(FR_E_LIMIT, fr_file_read_limited(file.c_str(), &out, 5, &err_));
    fr_buf_destroy(&out);
}

TEST_F(FdTest, EmptyFileRoundTrip) {
    const std::string file = path("empty.bin");
    ASSERT_EQ(FR_OK, fr_fd_open_write(file.c_str(), false, &fd_, &err_));
    ASSERT_EQ(FR_OK, fr_fd_close(&fd_, &err_));

    uint64_t size = 99;
    ASSERT_EQ(FR_OK, fr_file_size(file.c_str(), &size, &err_));
    EXPECT_EQ(0u, size);

    fr_buf out;
    fr_buf_init(&out);
    ASSERT_EQ(FR_OK, fr_file_read_limited(file.c_str(), &out, 16, &err_));
    EXPECT_EQ(0u, out.len);
    fr_buf_destroy(&out);
}

} // namespace
