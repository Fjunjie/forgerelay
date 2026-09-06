/*
 * fr_fd.c - fd 与文件系统原语实现。
 *
 * 平台策略（DECISIONS D-08）：POSIX 为主分支；_WIN32 分支仅服务于开发验证环境，
 * 映射到 CRT/Win32 等价物（fsync→_commit、rename→MoveFileExW 替换语义、
 * 目录同步为受控 no-op、二进制模式读写）。平台差异不出本文件。
 */
#include "forgerelay/fr_fd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "forgerelay/fr_checked.h"
#include "forgerelay/fr_path.h"

#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct stat fr_stat;
typedef ssize_t fr_ssize;

static fr_status map_errno(int e, fr_error *err, const char *what) {
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        fr_error_set_errno(err, FR_E_NOTFOUND, e, what);
        return FR_E_NOTFOUND;
    case EEXIST:
        fr_error_set_errno(err, FR_E_EXISTS, e, what);
        return FR_E_EXISTS;
    default:
        fr_error_set_errno(err, FR_E_IO, e, what);
        return FR_E_IO;
    }
}

#else /* _WIN32 */

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>

/** 宽字符路径缓冲容量（开发分支专用）。 */
#define FR_PATH_BUF_MAX_WIN 1024

typedef struct __stat64 fr_stat;
typedef long long fr_ssize;

static fr_status map_errno(int e, fr_error *err, const char *what) {
    switch (e) {
    case ENOENT:
    case ENOTDIR:
        fr_error_set_errno(err, FR_E_NOTFOUND, e, what);
        return FR_E_NOTFOUND;
    case EEXIST:
        fr_error_set_errno(err, FR_E_EXISTS, e, what);
        return FR_E_EXISTS;
    default:
        fr_error_set_errno(err, FR_E_IO, e, what);
        return FR_E_IO;
    }
}

/* UTF-8 路径转宽字符；失败返回 false。仅用于需要替换语义的 rename。 */
static bool win_utf8_to_wide(const char *s, wchar_t *out, size_t out_chars) {
    if (s == NULL || out == NULL) {
        return false;
    }
    int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (need <= 0 || (size_t)need > out_chars) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, need) == need;
}

#endif /* _WIN32 */

/* 通用：临时文件原子计数（D-09：唯一性而非机密性）。 */
#include <stdatomic.h>

static uint32_t fd_tmp_counter_next(void) {
    static atomic_uint counter;
    return atomic_fetch_add(&counter, 1u);
}

static bool tmp_prefix_valid(const char *prefix) {
    if (prefix == NULL) {
        return true;
    }
    size_t len = strlen(prefix);
    if (len > 32) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = prefix[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

fr_status fr_fd_open_read(const char *path, fr_fd *out, fr_error *err) {
    if (path == NULL || out == NULL) {
        fr_error_set(err, FR_E_ARG, "open_read: null argument");
        return FR_E_ARG;
    }
#ifndef _WIN32
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
#else
    int fd = _open(path, _O_RDONLY | _O_BINARY);
#endif
    if (fd < 0) {
        return map_errno(errno, err, "open read");
    }
    *out = fd;
    return FR_OK;
}

fr_status fr_fd_open_write(const char *path, bool exclusive, fr_fd *out, fr_error *err) {
    if (path == NULL || out == NULL) {
        fr_error_set(err, FR_E_ARG, "open_write: null argument");
        return FR_E_ARG;
    }
#ifndef _WIN32
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    if (exclusive) {
        flags |= O_EXCL;
    }
    /* 0600：仅属主可读写（SEC-03）。 */
    int fd = open(path, flags, 0600);
#else
    int flags = _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY;
    if (exclusive) {
        flags |= _O_EXCL;
    }
    int fd = _open(path, flags, _S_IREAD | _S_IWRITE);
#endif
    if (fd < 0) {
        return map_errno(errno, err, "open write");
    }
    *out = fd;
    return FR_OK;
}

fr_status fr_fd_close(fr_fd *fd, fr_error *err) {
    if (fd == NULL) {
        fr_error_set(err, FR_E_ARG, "close: null fd pointer");
        return FR_E_ARG;
    }
    if (*fd == FR_FD_INVALID) {
        return FR_OK; /* 幂等（LIFE-02）。 */
    }
    int rc;
#ifndef _WIN32
    do {
        rc = close(*fd);
    } while (rc != 0 && errno == EINTR);
    /* Linux 上 close 返回 EINTR 时 fd 已关闭；一律视为已复位。 */
#else
    rc = _close(*fd);
#endif
    *fd = FR_FD_INVALID;
    if (rc != 0) {
        return map_errno(errno, err, "close");
    }
    return FR_OK;
}

fr_status fr_fd_read(fr_fd fd, void *buf, size_t n, size_t *nread, fr_error *err) {
    if (nread == NULL || (buf == NULL && n != 0)) {
        fr_error_set(err, FR_E_ARG, "read: null argument");
        return FR_E_ARG;
    }
    *nread = 0;
    for (;;) {
#ifndef _WIN32
        fr_ssize r = read(fd, buf, n);
#else
        /* CRT 单次读取以 int 计量；上限裁剪到 1 GiB。 */
        size_t take = n;
        if (take > (size_t)0x40000000u) {
            take = (size_t)0x40000000u;
        }
        fr_ssize r = _read(fd, buf, (unsigned)take);
#endif
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return map_errno(errno, err, "read");
        }
        *nread = (size_t)r;
        return FR_OK;
    }
}

fr_status fr_fd_write_all(fr_fd fd, const void *buf, size_t n, fr_error *err) {
    if (buf == NULL && n != 0) {
        fr_error_set(err, FR_E_ARG, "write_all: null buffer");
        return FR_E_ARG;
    }
    const char *bytes = (const char *)buf;
    size_t off = 0;
    while (off < n) {
#ifndef _WIN32
        fr_ssize w = write(fd, bytes + off, n - off);
#else
        size_t take = n - off;
        if (take > (size_t)0x40000000u) {
            take = (size_t)0x40000000u;
        }
        fr_ssize w = _write(fd, bytes + off, (unsigned)take);
#endif
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return map_errno(errno, err, "write");
        }
        if (w == 0) {
            fr_error_set(err, FR_E_IO, "write returned 0");
            return FR_E_IO;
        }
        off += (size_t)w;
    }
    return FR_OK;
}

fr_status fr_fd_read_full(fr_fd fd, void *buf, size_t n, fr_error *err) {
    if (buf == NULL && n != 0) {
        fr_error_set(err, FR_E_ARG, "read_full: null buffer");
        return FR_E_ARG;
    }
    char *bytes = (char *)buf;
    size_t done = 0;
    while (done < n) {
        size_t chunk = 0;
        fr_status st = fr_fd_read(fd, bytes + done, n - done, &chunk, err);
        if (st != FR_OK) {
            return st;
        }
        if (chunk == 0) {
            fr_error_set(err, FR_E_EOF, "read_full: got %zu of %zu bytes", done, n);
            return FR_E_EOF;
        }
        done += chunk;
    }
    return FR_OK;
}

fr_status fr_fd_seek_set(fr_fd fd, uint64_t offset, fr_error *err) {
#ifndef _WIN32
    off_t target = (off_t)offset;
    if ((uint64_t)target != offset) {
        fr_error_set(err, FR_E_RANGE, "seek offset exceeds off_t");
        return FR_E_RANGE;
    }
    off_t r = lseek(fd, target, SEEK_SET);
#else
    __int64 target = (__int64)offset;
    __int64 r = _lseeki64(fd, target, SEEK_SET);
#endif
    if (r < 0 || (uint64_t)r != offset) {
        return map_errno(errno != 0 ? errno : EINVAL, err, "lseek");
    }
    return FR_OK;
}

fr_status fr_fd_fsync(fr_fd fd, fr_error *err) {
#ifndef _WIN32
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        return map_errno(errno, err, "fsync");
    }
    return FR_OK;
#else
    if (_commit(fd) != 0) {
        return map_errno(errno, err, "fsync");
    }
    return FR_OK;
#endif
}

fr_status fr_file_rename(const char *from, const char *to, fr_error *err) {
    if (from == NULL || to == NULL) {
        fr_error_set(err, FR_E_ARG, "rename: null path");
        return FR_E_ARG;
    }
#ifndef _WIN32
    if (rename(from, to) != 0) {
        return map_errno(errno, err, "rename");
    }
    return FR_OK;
#else
    wchar_t wfrom[FR_PATH_BUF_MAX_WIN], wto[FR_PATH_BUF_MAX_WIN];
    if (!win_utf8_to_wide(from, wfrom, FR_PATH_BUF_MAX_WIN) ||
        !win_utf8_to_wide(to, wto, FR_PATH_BUF_MAX_WIN)) {
        fr_error_set(err, FR_E_RANGE, "rename: path too long or invalid encoding");
        return FR_E_RANGE;
    }
    if (!MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD gle = GetLastError();
        if (gle == ERROR_FILE_NOT_FOUND || gle == ERROR_PATH_NOT_FOUND) {
            fr_error_set(err, FR_E_NOTFOUND, "rename: source not found");
            return FR_E_NOTFOUND;
        }
        fr_error_set(err, FR_E_IO, "rename: MoveFileExW failed (gle=%lu)", (unsigned long)gle);
        return FR_E_IO;
    }
    return FR_OK;
#endif
}

fr_status fr_file_unlink(const char *path, fr_error *err) {
    if (path == NULL) {
        fr_error_set(err, FR_E_ARG, "unlink: null path");
        return FR_E_ARG;
    }
#ifndef _WIN32
    if (unlink(path) != 0) {
        return map_errno(errno, err, "unlink");
    }
    return FR_OK;
#else
    if (_unlink(path) != 0) {
        return map_errno(errno, err, "unlink");
    }
    return FR_OK;
#endif
}

fr_status fr_file_exists(const char *path, bool *out, fr_error *err) {
    if (path == NULL || out == NULL) {
        fr_error_set(err, FR_E_ARG, "exists: null argument");
        return FR_E_ARG;
    }
    fr_stat st;
    memset(&st, 0, sizeof(st));
#ifndef _WIN32
    if (stat(path, &st) != 0) {
#else
    if (_stat64(path, &st) != 0) {
#endif
        if (errno == ENOENT || errno == ENOTDIR) {
            *out = false;
            return FR_OK;
        }
        return map_errno(errno, err, "stat");
    }
    *out = true;
    return FR_OK;
}

fr_status fr_file_size(const char *path, uint64_t *out, fr_error *err) {
    if (path == NULL || out == NULL) {
        fr_error_set(err, FR_E_ARG, "file_size: null argument");
        return FR_E_ARG;
    }
    fr_stat st;
    memset(&st, 0, sizeof(st));
#ifndef _WIN32
    if (stat(path, &st) != 0) {
#else
    if (_stat64(path, &st) != 0) {
#endif
        return map_errno(errno, err, "stat");
    }
    if (!S_ISREG(st.st_mode)) {
        fr_error_set(err, FR_E_RANGE, "file_size: not a regular file");
        return FR_E_RANGE;
    }
    if (st.st_size < 0) {
        fr_error_set(err, FR_E_IO, "file_size: negative size");
        return FR_E_IO;
    }
    *out = (uint64_t)st.st_size;
    return FR_OK;
}

fr_status fr_file_read_limited(const char *path, fr_buf *out, size_t max_bytes, fr_error *err) {
    if (path == NULL || out == NULL) {
        fr_error_set(err, FR_E_ARG, "read_limited: null argument");
        return FR_E_ARG;
    }
    fr_fd fd = FR_FD_INVALID;
    fr_status st = fr_fd_open_read(path, &fd, err);
    if (st != FR_OK) {
        return st;
    }

    fr_stat stbuf;
    memset(&stbuf, 0, sizeof(stbuf));
#ifndef _WIN32
    if (fstat(fd, &stbuf) != 0) {
#else
    if (_fstat64(fd, &stbuf) != 0) {
#endif
        int saved = errno;
        (void)fr_fd_close(&fd, NULL);
        return map_errno(saved, err, "fstat");
    }
    if (!S_ISREG(stbuf.st_mode)) {
        (void)fr_fd_close(&fd, NULL);
        fr_error_set(err, FR_E_RANGE, "read_limited: not a regular file");
        return FR_E_RANGE;
    }
    if (stbuf.st_size < 0 || (uint64_t)stbuf.st_size > (uint64_t)max_bytes) {
        (void)fr_fd_close(&fd, NULL);
        fr_error_set(err, FR_E_LIMIT, "read_limited: file exceeds limit");
        return FR_E_LIMIT;
    }

    size_t size = 0;
    if (!fr_size_from_u64((uint64_t)stbuf.st_size, &size)) {
        (void)fr_fd_close(&fd, NULL);
        fr_error_set(err, FR_E_LIMIT, "read_limited: file exceeds addressable size");
        return FR_E_LIMIT;
    }

    fr_buf_clear(out);
    st = fr_buf_reserve(out, size);
    if (st != FR_OK) {
        (void)fr_fd_close(&fd, NULL);
        fr_error_set(err, st, "read_limited: buffer reserve failed");
        return st;
    }
    st = fr_fd_read_full(fd, out->data, size, err);
    if (st != FR_OK) {
        (void)fr_fd_close(&fd, NULL);
        fr_buf_clear(out);
        return st;
    }
    out->len = size;
    return fr_fd_close(&fd, err);
}

fr_status fr_dir_create(const char *path, fr_error *err) {
    if (path == NULL || path[0] == '\0') {
        fr_error_set(err, FR_E_ARG, "mkdir: null or empty path");
        return FR_E_ARG;
    }
#ifndef _WIN32
    if (mkdir(path, 0700) == 0) { /* 0700：仅属主可访问（SEC-03）。 */
        return FR_OK;
    }
#else
    if (_mkdir(path) == 0) {
        return FR_OK;
    }
#endif
    if (errno == EEXIST) {
        fr_stat st;
        memset(&st, 0, sizeof(st));
#ifndef _WIN32
        if (stat(path, &st) != 0) {
#else
        if (_stat64(path, &st) != 0) {
#endif
            return map_errno(errno, err, "mkdir stat");
        }
        if (S_ISDIR(st.st_mode)) {
            return FR_OK; /* 幂等（LIFE-02）。 */
        }
        fr_error_set(err, FR_E_EXISTS, "mkdir: path exists and is not a directory");
        return FR_E_EXISTS;
    }
    return map_errno(errno, err, "mkdir");
}

fr_status fr_dir_create_all(const char *path, fr_error *err) {
    if (path == NULL || path[0] == '\0') {
        fr_error_set(err, FR_E_ARG, "mkdir_all: null or empty path");
        return FR_E_ARG;
    }
    size_t len = strlen(path);
    if (len >= FR_PATH_BUF_MAX) {
        fr_error_set(err, FR_E_RANGE, "mkdir_all: path too long");
        return FR_E_RANGE;
    }
    char buf[FR_PATH_BUF_MAX];
    memcpy(buf, path, len + 1);
#ifdef _WIN32
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\\') {
            buf[i] = '/';
        }
    }
    len = strlen(buf);
#endif

    size_t start = 0;
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0') {
            continue;
        }
        char saved = buf[i];
        buf[i] = '\0';
        /* 跳过空段与 Windows 盘符段（"C:"）。 */
        size_t seg_len = i - start;
        bool drive = seg_len == 2 && buf[start + 1] == ':';
        if (seg_len != 0 && !drive) {
            fr_status st = fr_dir_create(buf, err);
            if (st != FR_OK) {
                return st;
            }
        }
        buf[i] = saved;
        start = i + 1;
    }
    return FR_OK;
}

fr_status fr_dir_sync(const char *path, fr_error *err) {
    if (path == NULL) {
        fr_error_set(err, FR_E_ARG, "dir_sync: null path");
        return FR_E_ARG;
    }
#ifndef _WIN32
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return map_errno(errno, err, "dir open");
    }
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    int saved = errno;
    (void)close(fd);
    if (rc != 0) {
        return map_errno(saved, err, "dir fsync");
    }
    return FR_OK;
#else
    /* Windows 开发分支：无目录句柄 fsync 等价物；MoveFileExW 已带
     * MOVEFILE_WRITE_THROUGH，此处为受控 no-op（D-08）。
     * 仍校验目录存在性，保持与 POSIX 分支一致的状态语义。 */
    fr_stat st;
    memset(&st, 0, sizeof(st));
    if (_stat64(path, &st) != 0) {
        return map_errno(errno, err, "dir stat");
    }
    if (!S_ISDIR(st.st_mode)) {
        fr_error_set(err, FR_E_IO, "dir_sync: not a directory");
        return FR_E_IO;
    }
    return FR_OK;
#endif
}

fr_status fr_tmpfile_create(const char *dir, const char *prefix, char *name_out, size_t name_cap,
                            fr_fd *out, fr_error *err) {
    if (dir == NULL || name_out == NULL || out == NULL) {
        fr_error_set(err, FR_E_ARG, "tmpfile: null argument");
        return FR_E_ARG;
    }
    if (dir[0] == '\0') {
        fr_error_set(err, FR_E_ARG, "tmpfile: empty directory");
        return FR_E_ARG;
    }
    if (!tmp_prefix_valid(prefix)) {
        fr_error_set(err, FR_E_ARG, "tmpfile: invalid prefix");
        return FR_E_ARG;
    }
    if (name_cap < (size_t)FR_TMPNAME_MAX) {
        fr_error_set(err, FR_E_RANGE, "tmpfile: name buffer too small");
        return FR_E_RANGE;
    }

    const char *base = (prefix != NULL && prefix[0] != '\0') ? prefix : "t";
    long long pid = (long long)
#ifndef _WIN32
        getpid();
#else
        _getpid();
#endif
    long long tick = (long long)time(NULL);

    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t counter = fd_tmp_counter_next();
        char name[FR_TMPNAME_MAX];
        (void)snprintf(name, sizeof(name), "frtmp-%s-%lld-%lld-%u.part", base, pid, tick,
                       (unsigned)counter);
        char full[FR_PATH_BUF_MAX];
        int written = snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (written < 0 || (size_t)written >= sizeof(full)) {
            fr_error_set(err, FR_E_RANGE, "tmpfile: path too long");
            return FR_E_RANGE;
        }
        fr_fd fd = FR_FD_INVALID;
        fr_status st = fr_fd_open_write(full, true, &fd, err);
        if (st == FR_OK) {
            (void)memcpy(name_out, name, strlen(name) + 1);
            *out = fd;
            return FR_OK;
        }
        if (st != FR_E_EXISTS) {
            return st;
        }
        /* 名称冲突：换计数器重试。 */
    }
    fr_error_set(err, FR_E_IO, "tmpfile: retries exhausted");
    return FR_E_IO;
}
