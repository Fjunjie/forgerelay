/*
 * fr_frame.h - FRLY 二进制帧编解码（需求 §5.2、PROTO-01…05；格式见 docs/PROTOCOL.md）。
 *
 * 编码：将消息类型、标志、请求 ID 与负载编码为完整帧（20 字节头 + 负载）。
 * 解析：增量状态机，接受任意分片与多帧连续输入；头部完成时先校验全部字段
 *       与上限，再为负载分配内存（PROTO-02）；完整帧的负载拷贝进解析器
 *       自有缓冲，不指向调用方接收缓冲区（PROTO-05，D-02）。
 *
 * 所有权：fr_frame_parser 拥有内部 payload 缓冲，须 fr_frame_parser_destroy() 释放。
 * 线程安全：单个解析器实例非线程安全；编码函数为纯函数。无全局可变状态。
 * 错误语义：feed() 返回 FR_E_PROTOCOL 后解析器处于未定义对齐状态，
 *          调用方必须重新初始化或关闭连接（不可继续喂入）。
 */
#ifndef FR_FRAME_H
#define FR_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "forgerelay/fr_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 帧魔数 'FRLY'。 */
#define FR_FRAME_MAGIC "FRLY"
/** 帧魔数长度。 */
#define FR_FRAME_MAGIC_LEN 4
/** 协议版本（首版）。 */
#define FR_FRAME_VERSION 1
/** 固定头长度（含 CRC）。 */
#define FR_FRAME_HEADER_SIZE 20
/** 负载长度上限（8 MiB，需求 §7.3）。 */
#define FR_FRAME_MAX_PAYLOAD (8u * 1024u * 1024u)
/** 单帧最大长度（头 + 负载上限）。 */
#define FR_FRAME_MAX_FRAME (FR_FRAME_HEADER_SIZE + (size_t)FR_FRAME_MAX_PAYLOAD)

/** 消息类型（数值分配见 docs/PROTOCOL.md §3，D-07）。 */
typedef enum fr_msg_type {
    FR_MSG_HELLO = 1,            /**< 连接：版本协商。 */
    FR_MSG_AUTH = 2,             /**< 连接：认证。 */
    FR_MSG_PING = 3,             /**< 连接：探活。 */
    FR_MSG_CLOSE = 4,            /**< 连接：优雅关闭。 */
    FR_MSG_CREATE_UPLOAD = 16,   /**< 上传：创建会话。 */
    FR_MSG_PUT_CHUNK = 17,       /**< 上传：上传数据块。 */
    FR_MSG_QUERY_UPLOAD = 18,    /**< 上传：查询缺失块。 */
    FR_MSG_COMMIT_UPLOAD = 19,   /**< 上传：提交会话。 */
    FR_MSG_ABORT_UPLOAD = 20,    /**< 上传：终止会话。 */
    FR_MSG_GET_ARTIFACT = 32,    /**< 下载：请求制品/范围。 */
    FR_MSG_DATA = 33,            /**< 下载：数据流。 */
    FR_MSG_LIST_ARTIFACTS = 48,  /**< 查询：分页列出。 */
    FR_MSG_SHOW_ARTIFACT = 49,   /**< 查询：查看元数据。 */
    FR_MSG_STATUS = 50,          /**< 查询：服务状态。 */
    FR_MSG_DELETE_ARTIFACT = 64, /**< 管理：删除制品。 */
    FR_MSG_RUN_GC = 65,          /**< 管理：执行清理。 */
    FR_MSG_USER_ADD = 66,        /**< 管理：添加用户。 */
    FR_MSG_USER_DISABLE = 67,    /**< 管理：禁用用户。 */
    FR_MSG_USER_LIST = 68,       /**< 管理：列出用户。 */
    FR_MSG_TOKEN_CREATE = 80,    /**< 管理：创建令牌。 */
    FR_MSG_TOKEN_REVOKE = 81,    /**< 管理：撤销令牌。 */
    FR_MSG_TOKEN_LIST = 82,      /**< 管理：列出令牌。 */
    FR_MSG_SESSION_LIST = 96,    /**< 管理：列出上传会话。 */
    FR_MSG_SESSION_ABORT = 97,   /**< 管理：终止上传会话。 */
    FR_MSG_OK = 200,             /**< 通用：成功响应。 */
    FR_MSG_ERROR = 201           /**< 通用：错误响应。 */
} fr_msg_type;

/**
 * 校验消息类型是否在白名单内（PROTO-04）。
 *
 * @param[in] type 消息类型值。
 * @return true 合法。
 */
bool fr_msg_type_valid(unsigned type);

/**
 * 编码完整帧并追加到 out（§5.2 布局）。
 *
 * @param[out] out 目标缓冲（追加）。
 * @param[in] type 消息类型；必须在白名单内。
 * @param[in] flags 标志位；首版必须为 0（D-06）。
 * @param[in] req_id 请求 ID；不得为 0（D-05）。
 * @param[in] payload 负载数据；payload_len == 0 时可为 NULL。
 * @param[in] payload_len 负载长度；<= FR_FRAME_MAX_PAYLOAD。
 * @retval FR_OK 成功，out 追加 20 + payload_len 字节。
 * @retval FR_E_ARG out 为 NULL，或 type/flags/req_id/payload 组合非法。
 * @retval FR_E_RANGE payload_len 超过 FR_FRAME_MAX_PAYLOAD。
 * @retval FR_E_NOMEM out 扩容失败。
 */
fr_status fr_frame_encode(fr_buf *out, uint8_t type, uint16_t flags, uint32_t req_id,
                          const void *payload, size_t payload_len);

/** 解析产出的完整帧（只读结果）。 */
typedef struct fr_frame {
    uint8_t type;           /**< 消息类型（已过白名单）。 */
    uint16_t flags;         /**< 标志位（必为 0）。 */
    uint32_t req_id;        /**< 请求 ID（非 0）。 */
    const uint8_t *payload; /**< 负载；指向解析器内部缓冲，**有效期到下一次 feed()/destroy()**。
                                 payload_len == 0 时为 NULL。 */
    size_t payload_len;     /**< 负载长度。 */
} fr_frame;

/** 增量帧解析器。 */
typedef struct fr_frame_parser {
    fr_buf payload_buf; /**< 内部负载缓冲（拥有）；当前帧完成后供 fr_frame.payload 引用。 */
    uint8_t header[FR_FRAME_HEADER_SIZE]; /**< 头部累积区。 */
    size_t header_have;                   /**< 已累积的头部字节数（0..FR_FRAME_HEADER_SIZE）。 */
    bool header_done;                     /**< 头部已完整并通过全部校验。 */
    uint8_t type;                         /**< 头部解析出的消息类型（header_done 后有效）。 */
    uint16_t flags;                       /**< 头部解析出的标志位（header_done 后有效）。 */
    uint32_t req_id;                      /**< 头部解析出的请求 ID（header_done 后有效）。 */
    uint32_t payload_need;                /**< 头部声明的负载长度（header_done 后有效）。 */
} fr_frame_parser;

/**
 * 初始化解析器（不分配内存；首次负载到达时惰性分配）。
 *
 * @param[out] p 解析器。
 */
void fr_frame_parser_init(fr_frame_parser *p);

/**
 * 释放解析器内部缓冲并复位为初始状态；可安全重复调用。
 *
 * @param[in,out] p 解析器。
 */
void fr_frame_parser_destroy(fr_frame_parser *p);

/**
 * 喂入一段接收数据，尝试产出一个完整帧。
 *
 * 分片语义（PROTO-01）：输入可任意分片；不完整时所有输入被吸收，
 * *consumed == len 且 *frame_ready == false。一帧完成后立即返回，
 * 剩余输入由调用方在下次调用中传入（*consumed < len）。
 *
 * @param[in,out] p 解析器。
 * @param[in] data 输入数据；len == 0 时可为 NULL。
 * @param[in] len 输入长度。
 * @param[out] consumed 实际吸收的字节数；出错时不修改。
 * @param[out] out 完整帧结果；仅当 *frame_ready == true 时有效。
 * @param[out] frame_ready 是否产出完整帧。
 * @retval FR_OK 正常（无论是否产出帧）。
 * @retval FR_E_ARG 必要参数为 NULL。
 * @retval FR_E_PROTOCOL 头部校验失败（PROTO-04）；此后解析器不可继续使用。
 */
fr_status fr_frame_parser_feed(fr_frame_parser *p, const void *data, size_t len, size_t *consumed,
                               fr_frame *out, bool *frame_ready);

#ifdef __cplusplus
}
#endif

#endif /* FR_FRAME_H */
