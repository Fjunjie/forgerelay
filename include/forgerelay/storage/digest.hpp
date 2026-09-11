// fr/digest.hpp - SHA-256 流式摘要（FR-STO-01、SEC-01）。
//
// 算法实现来自系统加密 API，本项目不自研密码算法（SEC-01）：
//   - Linux（目标平台）：OpenSSL 3.x EVP（构建需 find_package(OpenSSL)）；
//   - Windows（开发验证环境）：系统 BCrypt（CNG）API（D-13）。
//
// 线程安全：单个 Sha256Stream 实例非线程安全；无共享全局状态。
#ifndef FR_STORAGE_DIGEST_HPP
#define FR_STORAGE_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fr {

/** SHA-256 摘要长度（字节）。 */
constexpr size_t kSha256Len = 32;

/** SHA-256 摘要二进制值。 */
using Sha256 = std::array<uint8_t, kSha256Len>;

/** SHA-256 流式计算器。 */
class Sha256Stream {
public:
    Sha256Stream();
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream &) = delete;
    Sha256Stream &operator=(const Sha256Stream &) = delete;
    Sha256Stream(Sha256Stream &&other) noexcept;
    Sha256Stream &operator=(Sha256Stream &&other) noexcept;

    /** 追加数据；len == 0 时为无害操作。 */
    void update(const void *data, size_t len);

    /**
     * 结束计算并返回摘要。
     *
     * @return 32 字节 SHA-256；调用后流不可继续 update。
     */
    Sha256 finish();

private:
    void *impl_ = nullptr;  // 后端句柄（EVP_MD_CTX* 或 BCRYPT_HASH_HANDLE）
    void *state_ = nullptr; // 后端附加状态（对象缓冲等）
};

/**
 * 计算缓冲区的一次性 SHA-256。
 *
 * @param[in] data 输入数据；len == 0 时可为 nullptr（空输入摘要）。
 * @param[in] len 输入长度。
 * @return 摘要。
 */
Sha256 sha256(const void *data, size_t len);

/** 摘要转小写 hex（64 字符）。 */
std::string sha256_to_hex(const Sha256 &digest);

/**
 * 解析 64 位小写 hex 摘要（先经格式校验）。
 *
 * @param[in] hex 摘要字符串。
 * @return 二进制摘要；格式非法时抛 fr::Error(FR_E_ARG)。
 */
Sha256 sha256_from_hex(const std::string &hex);

/**
 * 生成 32 字节密码学安全随机数（SEC-01：来自系统加密 API，D-13）。
 *
 * @return 随机字节；系统 API 失败时抛 fr::Error(FR_E_INTERNAL)。
 */
Sha256 random_bytes();

} // namespace fr

#endif /* FR_STORAGE_DIGEST_HPP */
