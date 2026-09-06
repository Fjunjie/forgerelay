/*
 * fr_buffer.c - 可增长字节缓冲实现。
 */
#include "forgerelay/fr_buffer.h"

#include <stdlib.h>
#include <string.h>

#include "forgerelay/fr_byteorder.h"
#include "forgerelay/fr_checked.h"

void fr_buf_init(fr_buf *buf) {
    if (buf == NULL) {
        return;
    }
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void fr_buf_destroy(fr_buf *buf) {
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

void fr_buf_move(fr_buf *dst, fr_buf *src) {
    if (dst == NULL || src == NULL || dst == src) {
        return;
    }
    fr_buf_destroy(dst);
    dst->data = src->data;
    dst->len = src->len;
    dst->cap = src->cap;
    src->data = NULL;
    src->len = 0;
    src->cap = 0;
}

fr_status fr_buf_reserve(fr_buf *buf, size_t additional) {
    if (buf == NULL) {
        return FR_E_ARG;
    }
    size_t needed = 0;
    if (!fr_checked_add_size(buf->len, additional, &needed)) {
        return FR_E_OVERFLOW;
    }
    if (needed <= buf->cap) {
        return FR_OK;
    }

    /* 指数扩容：new_cap >= needed 且 >= 64，倍增至满足；全程溢出检查。 */
    size_t new_cap = buf->cap != 0 ? buf->cap : (size_t)64;
    while (new_cap < needed) {
        if (!fr_checked_mul_size(new_cap, (size_t)2, &new_cap)) {
            return FR_E_OVERFLOW;
        }
    }

    uint8_t *grown = realloc(buf->data, new_cap);
    if (grown == NULL) {
        return FR_E_NOMEM;
    }
    buf->data = grown;
    buf->cap = new_cap;
    return FR_OK;
}

fr_status fr_buf_append(fr_buf *buf, const void *src, size_t n) {
    if (buf == NULL) {
        return FR_E_ARG;
    }
    if (src == NULL && n != 0) {
        return FR_E_ARG;
    }
    fr_status st = fr_buf_reserve(buf, n);
    if (st != FR_OK) {
        return st;
    }
    if (n != 0) {
        memcpy(buf->data + buf->len, src, n);
    }
    buf->len += n;
    return FR_OK;
}

fr_status fr_buf_append_u8(fr_buf *buf, uint8_t v) {
    return fr_buf_append(buf, &v, (size_t)1);
}

fr_status fr_buf_append_u16be(fr_buf *buf, uint16_t v) {
    uint8_t raw[2];
    fr_store_be16(raw, v);
    return fr_buf_append(buf, raw, sizeof(raw));
}

fr_status fr_buf_append_u32be(fr_buf *buf, uint32_t v) {
    uint8_t raw[4];
    fr_store_be32(raw, v);
    return fr_buf_append(buf, raw, sizeof(raw));
}

fr_status fr_buf_append_u64be(fr_buf *buf, uint64_t v) {
    uint8_t raw[8];
    fr_store_be64(raw, v);
    return fr_buf_append(buf, raw, sizeof(raw));
}

void fr_buf_clear(fr_buf *buf) {
    if (buf == NULL) {
        return;
    }
    buf->len = 0;
}

fr_status fr_buf_consume(fr_buf *buf, size_t n) {
    if (buf == NULL) {
        return FR_E_ARG;
    }
    if (n > buf->len) {
        return FR_E_RANGE;
    }
    if (n != 0) {
        memmove(buf->data, buf->data + n, buf->len - n);
        buf->len -= n;
    }
    return FR_OK;
}

fr_buf_view fr_buf_view_of(const fr_buf *buf) {
    fr_buf_view view = {NULL, 0};
    if (buf != NULL && buf->len != 0) {
        view.data = buf->data;
        view.len = buf->len;
    }
    return view;
}

void fr_buf_reader_init(fr_buf_reader *r, const void *data, size_t len) {
    if (r == NULL) {
        return;
    }
    r->data = (const uint8_t *)data;
    r->len = len;
    r->pos = 0;
}

size_t fr_buf_reader_remaining(const fr_buf_reader *r) {
    if (r == NULL || r->pos >= r->len) {
        return 0;
    }
    return r->len - r->pos;
}

fr_status fr_buf_reader_read(fr_buf_reader *r, void *dst, size_t n) {
    if (r == NULL || (dst == NULL && n != 0)) {
        return FR_E_ARG;
    }
    if (n > fr_buf_reader_remaining(r)) {
        return FR_E_RANGE;
    }
    if (n != 0) {
        memcpy(dst, r->data + r->pos, n);
    }
    r->pos += n;
    return FR_OK;
}

fr_status fr_buf_reader_u8(fr_buf_reader *r, uint8_t *out) {
    return fr_buf_reader_read(r, out, (size_t)1);
}

fr_status fr_buf_reader_u16be(fr_buf_reader *r, uint16_t *out) {
    uint8_t raw[2];
    fr_status st = fr_buf_reader_read(r, raw, sizeof(raw));
    if (st != FR_OK) {
        return st;
    }
    *out = fr_load_be16(raw);
    return FR_OK;
}

fr_status fr_buf_reader_u32be(fr_buf_reader *r, uint32_t *out) {
    uint8_t raw[4];
    fr_status st = fr_buf_reader_read(r, raw, sizeof(raw));
    if (st != FR_OK) {
        return st;
    }
    *out = fr_load_be32(raw);
    return FR_OK;
}

fr_status fr_buf_reader_u64be(fr_buf_reader *r, uint64_t *out) {
    uint8_t raw[8];
    fr_status st = fr_buf_reader_read(r, raw, sizeof(raw));
    if (st != FR_OK) {
        return st;
    }
    *out = fr_load_be64(raw);
    return FR_OK;
}

fr_status fr_buf_reader_skip(fr_buf_reader *r, size_t n) {
    if (r == NULL) {
        return FR_E_ARG;
    }
    if (n > fr_buf_reader_remaining(r)) {
        return FR_E_RANGE;
    }
    r->pos += n;
    return FR_OK;
}

fr_status fr_buf_reader_bytes(fr_buf_reader *r, fr_buf *out, size_t n) {
    if (r == NULL || out == NULL) {
        return FR_E_ARG;
    }
    if (n > fr_buf_reader_remaining(r)) {
        return FR_E_RANGE;
    }
    fr_status st = fr_buf_reserve(out, n);
    if (st != FR_OK) {
        return st;
    }
    if (n != 0) {
        memcpy(out->data + out->len, r->data + r->pos, n);
    }
    out->len += n;
    r->pos += n;
    return FR_OK;
}
