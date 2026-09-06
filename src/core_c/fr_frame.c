/*
 * fr_frame.c - FRLY 帧编解码实现。
 *
 * 头部布局（大端）：
 *   [0..3]  magic 'FRLY'
 *   [4]     version
 *   [5]     type
 *   [6..7]  flags（必须 0）
 *   [8..11] request id（非 0）
 *   [12..15] payload length（<= 8 MiB）
 *   [16..19] CRC32 of [0..15]
 */
#include "forgerelay/fr_frame.h"

#include <string.h>

#include "forgerelay/fr_byteorder.h"
#include "forgerelay/fr_checked.h"
#include "forgerelay/fr_crc32.h"
#include "forgerelay/fr_error.h"

bool fr_msg_type_valid(unsigned type) {
    switch (type) {
    case FR_MSG_HELLO:
    case FR_MSG_AUTH:
    case FR_MSG_PING:
    case FR_MSG_CLOSE:
    case FR_MSG_CREATE_UPLOAD:
    case FR_MSG_PUT_CHUNK:
    case FR_MSG_QUERY_UPLOAD:
    case FR_MSG_COMMIT_UPLOAD:
    case FR_MSG_ABORT_UPLOAD:
    case FR_MSG_GET_ARTIFACT:
    case FR_MSG_DATA:
    case FR_MSG_LIST_ARTIFACTS:
    case FR_MSG_SHOW_ARTIFACT:
    case FR_MSG_STATUS:
    case FR_MSG_DELETE_ARTIFACT:
    case FR_MSG_RUN_GC:
    case FR_MSG_USER_ADD:
    case FR_MSG_USER_DISABLE:
    case FR_MSG_USER_LIST:
    case FR_MSG_TOKEN_CREATE:
    case FR_MSG_TOKEN_REVOKE:
    case FR_MSG_TOKEN_LIST:
    case FR_MSG_SESSION_LIST:
    case FR_MSG_SESSION_ABORT:
    case FR_MSG_OK:
    case FR_MSG_ERROR:
        return true;
    default:
        return false;
    }
}

fr_status fr_frame_encode(fr_buf *out, uint8_t type, uint16_t flags, uint32_t req_id,
                          const void *payload, size_t payload_len) {
    if (out == NULL) {
        return FR_E_ARG;
    }
    if (!fr_msg_type_valid((unsigned)type)) {
        return FR_E_ARG;
    }
    if (flags != 0u) {
        return FR_E_ARG;
    }
    if (req_id == 0u) {
        return FR_E_ARG;
    }
    if (payload == NULL && payload_len != 0u) {
        return FR_E_ARG;
    }
    if (payload_len > (size_t)FR_FRAME_MAX_PAYLOAD) {
        return FR_E_RANGE;
    }

    uint8_t header[FR_FRAME_HEADER_SIZE];
    memcpy(header, FR_FRAME_MAGIC, FR_FRAME_MAGIC_LEN);
    header[4] = FR_FRAME_VERSION;
    header[5] = type;
    fr_store_be16(header + 6, flags);
    fr_store_be32(header + 8, req_id);
    fr_store_be32(header + 12, (uint32_t)payload_len);
    fr_store_be32(header + 16, fr_crc32(header, 16));

    size_t total = 0;
    if (!fr_checked_add_size((size_t)FR_FRAME_HEADER_SIZE, payload_len, &total)) {
        return FR_E_OVERFLOW;
    }
    fr_status st = fr_buf_reserve(out, total);
    if (st != FR_OK) {
        return st;
    }
    st = fr_buf_append(out, header, sizeof(header));
    if (st != FR_OK) {
        return st;
    }
    return fr_buf_append(out, payload, payload_len);
}

void fr_frame_parser_init(fr_frame_parser *p) {
    if (p == NULL) {
        return;
    }
    fr_buf_init(&p->payload_buf);
    memset(p->header, 0, sizeof(p->header));
    p->header_have = 0;
    p->header_done = false;
    p->type = 0;
    p->flags = 0;
    p->req_id = 0;
    p->payload_need = 0;
}

void fr_frame_parser_destroy(fr_frame_parser *p) {
    if (p == NULL) {
        return;
    }
    fr_buf_destroy(&p->payload_buf);
    fr_frame_parser_init(p);
}

/* 头部完成后的统一校验；失败时通过 err 描述具体原因（不含敏感信息，PROTO-06）。 */
static fr_status frame_validate_header(fr_frame_parser *p) {
    if (memcmp(p->header, FR_FRAME_MAGIC, FR_FRAME_MAGIC_LEN) != 0) {
        return FR_E_PROTOCOL;
    }
    if (p->header[4] != FR_FRAME_VERSION) {
        return FR_E_PROTOCOL;
    }
    p->type = p->header[5];
    p->flags = fr_load_be16(p->header + 6);
    p->req_id = fr_load_be32(p->header + 8);
    p->payload_need = fr_load_be32(p->header + 12);

    if (p->flags != 0u) {
        return FR_E_PROTOCOL;
    }
    if (p->req_id == 0u) {
        return FR_E_PROTOCOL;
    }
    if (!fr_msg_type_valid((unsigned)p->type)) {
        return FR_E_PROTOCOL;
    }
    if (p->payload_need > (uint32_t)FR_FRAME_MAX_PAYLOAD) {
        return FR_E_PROTOCOL;
    }
    uint32_t stored_crc = fr_load_be32(p->header + 16);
    if (stored_crc != fr_crc32(p->header, 16)) {
        return FR_E_PROTOCOL;
    }
    return FR_OK;
}

fr_status fr_frame_parser_feed(fr_frame_parser *p, const void *data, size_t len, size_t *consumed,
                               fr_frame *out, bool *frame_ready) {
    if (p == NULL || consumed == NULL || out == NULL || frame_ready == NULL) {
        return FR_E_ARG;
    }
    if (data == NULL && len != 0) {
        return FR_E_ARG;
    }

    *frame_ready = false;
    const uint8_t *bytes = (const uint8_t *)data;
    size_t off = 0;

    /* 阶段一：累积头部。 */
    while (!p->header_done && p->header_have < FR_FRAME_HEADER_SIZE && off < len) {
        size_t want = (size_t)FR_FRAME_HEADER_SIZE - p->header_have;
        size_t take = len - off < want ? len - off : want;
        memcpy(p->header + p->header_have, bytes + off, take);
        p->header_have += take;
        off += take;
        if (p->header_have == (size_t)FR_FRAME_HEADER_SIZE) {
            fr_status st = frame_validate_header(p);
            if (st != FR_OK) {
                return st; /* 解析器失效：调用方必须重新初始化或断开。 */
            }
            p->header_done = true;
        }
    }
    if (!p->header_done) {
        *consumed = off;
        return FR_OK;
    }

    /* 阶段二：累积负载（长度上限已在分配前校验，PROTO-02）。 */
    size_t need = (size_t)p->payload_need - p->payload_buf.len;
    size_t take = len - off < need ? len - off : need;
    if (take != 0) {
        fr_status st = fr_buf_append(&p->payload_buf, bytes + off, take);
        if (st != FR_OK) {
            return st;
        }
        off += take;
    }

    if (p->payload_buf.len != (size_t)p->payload_need) {
        *consumed = off;
        return FR_OK;
    }

    /* 帧完成：产出结果并复位，等待下一帧。 */
    out->type = p->type;
    out->flags = p->flags;
    out->req_id = p->req_id;
    out->payload_len = p->payload_buf.len;
    out->payload = p->payload_buf.len != 0 ? p->payload_buf.data : NULL;
    p->header_have = 0;
    p->header_done = false;
    p->type = 0;
    p->flags = 0;
    p->req_id = 0;
    p->payload_need = 0;
    fr_buf_clear(&p->payload_buf); /* 保留容量，payload 指针在下一次 feed 前仍有效（D-02）。 */
    *consumed = off;
    *frame_ready = true;
    return FR_OK;
}
