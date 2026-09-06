/*
 * fr_version.h - libfrcore 版本常量与协议版本。
 *
 * 所有声明均为线程安全的纯函数/常量。
 */
#ifndef FR_VERSION_H
#define FR_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/** libfrcore 主版本号。 */
#define FR_CORE_VERSION_MAJOR 0
/** libfrcore 次版本号。 */
#define FR_CORE_VERSION_MINOR 1
/** libfrcore 修订号。 */
#define FR_CORE_VERSION_PATCH 0

/** 传输协议版本（帧头 Version 字段，需求 §5.2）。 */
#define FR_PROTOCOL_VERSION 1

/**
 * 返回核心库版本字符串（形如 "libfrcore 0.1.0"）。
 *
 * @return 静态存储期字符串；线程安全。
 */
const char *fr_core_version_string(void);

#ifdef __cplusplus
}
#endif

#endif /* FR_VERSION_H */
