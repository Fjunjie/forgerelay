/*
 * fr_hex.c - hex 编解码实现。
 */
#include "forgerelay/fr_hex.h"

#include <string.h>

static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool is_lower_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

fr_status fr_hex_encode(const uint8_t *raw, size_t raw_len, char *out, size_t out_cap) {
    if (out == NULL) {
        return FR_E_ARG;
    }
    if (raw == NULL && raw_len != 0) {
        return FR_E_ARG;
    }
    size_t need = raw_len * 2u + 1u;
    if (out_cap < need) {
        return FR_E_RANGE;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < raw_len; i++) {
        out[i * 2u] = digits[(raw[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = digits[raw[i] & 0x0fu];
    }
    out[raw_len * 2u] = '\0';
    return FR_OK;
}

fr_status fr_hex_decode(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (hex == NULL || out == NULL) {
        return FR_E_ARG;
    }
    size_t hex_len = strlen(hex);
    if ((hex_len % 2u) != 0u) {
        return FR_E_ARG;
    }
    if (out_cap < hex_len / 2u) {
        return FR_E_RANGE;
    }
    for (size_t i = 0; i < hex_len; i += 2u) {
        int hi = hex_digit_value(hex[i]);
        int lo = hex_digit_value(hex[i + 1u]);
        if (hi < 0 || lo < 0) {
            return FR_E_ARG;
        }
        out[i / 2u] = (uint8_t)((hi << 4) | lo);
    }
    if (out_len != NULL) {
        *out_len = hex_len / 2u;
    }
    return FR_OK;
}

bool fr_digest_hex_valid(const char *s) {
    if (s == NULL) {
        return false;
    }
    if (strlen(s) != FR_SHA256_HEX_LEN) {
        return false;
    }
    for (size_t i = 0; i < FR_SHA256_HEX_LEN; i++) {
        if (!is_lower_hex(s[i])) {
            return false;
        }
    }
    return true;
}
