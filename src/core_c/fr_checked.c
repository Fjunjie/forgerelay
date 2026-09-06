/*
 * fr_checked.c - 溢出检查算术实现（PROTO-03）。
 *
 * 优先使用编译器溢出内建函数；结果先写入局部变量，成功才写 *out，
 * 以保证“失败不修改出参”的契约（内建函数溢出时也会写入环绕值）。
 */
#include "forgerelay/fr_checked.h"

#if defined(__GNUC__) && !defined(FR_NO_BUILTIN_OVERFLOW)
#define FR_HAVE_BUILTIN_OVERFLOW 1
#endif

bool fr_checked_add_u32(uint32_t a, uint32_t b, uint32_t *out) {
    uint32_t r = 0;
#if defined(FR_HAVE_BUILTIN_OVERFLOW)
    if (__builtin_add_overflow(a, b, &r)) {
        return false;
    }
#else
    const uint64_t wide = (uint64_t)a + (uint64_t)b;
    if (wide > (uint64_t)UINT32_MAX) {
        return false;
    }
    r = (uint32_t)wide;
#endif
    if (out != NULL) {
        *out = r;
    }
    return true;
}

bool fr_checked_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    uint64_t r = 0;
#if defined(FR_HAVE_BUILTIN_OVERFLOW)
    if (__builtin_add_overflow(a, b, &r)) {
        return false;
    }
#else
    r = a + b; /* 无符号环绕语义 */
    if (r < a) {
        return false;
    }
#endif
    if (out != NULL) {
        *out = r;
    }
    return true;
}

bool fr_checked_sub_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a < b) {
        return false;
    }
    if (out != NULL) {
        *out = a - b;
    }
    return true;
}

bool fr_checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    uint64_t r = 0;
#if defined(FR_HAVE_BUILTIN_OVERFLOW)
    if (__builtin_mul_overflow(a, b, &r)) {
        return false;
    }
#else
    if (a == 0u || b == 0u) {
        r = 0;
    } else {
        r = a * b;
        if (r / b != a) {
            return false;
        }
    }
#endif
    if (out != NULL) {
        *out = r;
    }
    return true;
}

bool fr_size_from_u64(uint64_t v, size_t *out) {
    if (v > (uint64_t)SIZE_MAX) {
        return false;
    }
    if (out != NULL) {
        *out = (size_t)v;
    }
    return true;
}

bool fr_checked_add_size(size_t a, size_t b, size_t *out) {
    uint64_t wide = 0;
    if (!fr_checked_add_u64((uint64_t)a, (uint64_t)b, &wide)) {
        return false;
    }
    return fr_size_from_u64(wide, out);
}

bool fr_checked_sub_size(size_t a, size_t b, size_t *out) {
    uint64_t wide = 0;
    if (!fr_checked_sub_u64((uint64_t)a, (uint64_t)b, &wide)) {
        return false;
    }
    return fr_size_from_u64(wide, out);
}

bool fr_checked_mul_size(size_t a, size_t b, size_t *out) {
    uint64_t wide = 0;
    if (!fr_checked_mul_u64((uint64_t)a, (uint64_t)b, &wide)) {
        return false;
    }
    return fr_size_from_u64(wide, out);
}
