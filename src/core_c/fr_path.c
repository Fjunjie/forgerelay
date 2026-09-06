/*
 * fr_path.c - 标识校验与路径映射实现。
 */
#include "forgerelay/fr_path.h"

#include <string.h>

#include "forgerelay/fr_checked.h"
#include "forgerelay/fr_hex.h"

static bool is_ident_component_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
}

static bool is_version_extra_char(char c) {
    return c == '+' || c == '_';
}

static bool valid_dot_segment_free(const char *s) {
    /* 拒绝整段 "." 与 ".."，避免任何形式的路径穿越表达。 */
    return strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

bool fr_ident_valid_component(const char *s) {
    if (s == NULL) {
        return false;
    }
    size_t len = strlen(s);
    if (len < 1 || len > FR_IDENT_COMPONENT_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!is_ident_component_char(s[i])) {
            return false;
        }
    }
    return valid_dot_segment_free(s);
}

bool fr_ident_valid_version(const char *s) {
    if (s == NULL) {
        return false;
    }
    size_t len = strlen(s);
    if (len < 1 || len > FR_IDENT_VERSION_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!is_ident_component_char(c) && !is_version_extra_char(c)) {
            return false;
        }
    }
    return valid_dot_segment_free(s);
}

fr_status fr_chunk_rel_path(const char *hex_digest, char *out, size_t out_cap) {
    if (out == NULL) {
        return FR_E_ARG;
    }
    if (!fr_digest_hex_valid(hex_digest)) {
        return FR_E_ARG;
    }
    if (out_cap < (size_t)FR_CHUNK_REL_PATH_LEN) {
        return FR_E_RANGE;
    }
    out[0] = hex_digest[0];
    out[1] = hex_digest[1];
    out[2] = '/';
    memcpy(out + 3, hex_digest + 2, 62);
    out[3 + 62] = '\0';
    return FR_OK;
}

fr_status fr_path_validate_rel(const char *rel) {
    if (rel == NULL) {
        return FR_E_ARG;
    }
    size_t len = strlen(rel);
    if (len == 0) {
        return FR_E_RANGE;
    }
    if (rel[0] == '/' || rel[0] == '\\') {
        return FR_E_RANGE;
    }
    for (size_t i = 0; i < len; i++) {
        char c = rel[i];
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_' || c == '-' || c == '/';
        if (!allowed) {
            return FR_E_RANGE;
        }
        if (c == '/') {
            /* 禁止空分量 "//" 与末尾 '/'。 */
            if (i + 1 == len || rel[i + 1] == '/') {
                return FR_E_RANGE;
            }
        }
    }
    /* 逐分量拒绝 "." / ".."。 */
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (rel[i] == '/' || rel[i] == '\0') {
            size_t comp_len = i - start;
            if (comp_len == 1 && rel[start] == '.') {
                return FR_E_RANGE;
            }
            if (comp_len == 2 && rel[start] == '.' && rel[start + 1] == '.') {
                return FR_E_RANGE;
            }
            start = i + 1;
        }
    }
    return FR_OK;
}

fr_status fr_path_join(const char *root, const char *rel, char *out, size_t out_cap) {
    if (root == NULL || rel == NULL || out == NULL) {
        return FR_E_ARG;
    }
    if (fr_path_validate_rel(rel) != FR_OK) {
        return FR_E_RANGE;
    }
    size_t root_len = strlen(root);
    if (root_len == 0) {
        return FR_E_RANGE;
    }
    bool root_is_fs_root = false;
    /* 容忍 root 末尾的 '/'，统一按单分隔符拼接。 */
    if (root[root_len - 1] == '/') {
        root_len -= 1;
        if (root_len == 0) {
            /* 根目录本身即 "/"：不再补分隔符，直接拼接。 */
            root_is_fs_root = true;
            root_len = 1;
        }
    }
    size_t rel_len = strlen(rel);
    size_t total = 0;
    if (!fr_checked_add_size(root_len, rel_len, &total) || !fr_checked_add_size(total, 1, &total)) {
        return FR_E_OVERFLOW;
    }
    if (!root_is_fs_root && !fr_checked_add_size(total, 1, &total)) {
        return FR_E_OVERFLOW;
    }
    if (out_cap < total) {
        return FR_E_RANGE;
    }
    memcpy(out, root, root_len);
    size_t pos = root_len;
    if (!root_is_fs_root) {
        out[pos++] = '/';
    }
    memcpy(out + pos, rel, rel_len);
    out[pos + rel_len] = '\0';
    return FR_OK;
}
