/**
 ******************************************************************************
 * @file    crc32.h
 * @brief   IEEE 802.3 标准 CRC-32（用于整包固件完整性校验）
 * @note    SafeBoot-F1
 *
 * 为什么分包用 CRC16、整包用 CRC32：
 *   单包 128 字节在串口线上跑，CRC16 占 2 字节开销小；
 *   整包固件 8~24 KB，跨数十个扇区，用标准 CRC32（与 Python zlib.crc32 完全一致）
 *   可以直接在上位机和固件之间做字节级金标比对，漏检率 < 2^-32。
 ******************************************************************************
 */
#ifndef __CRC32_H
#define __CRC32_H

#include "common.h"

/**
 * @brief  增量计算 CRC32（首次 state 传 0xFFFFFFFFu，最后取反，或直接调 crc32_calc）
 */
uint32_t crc32_update(uint32_t crc_accum, const uint8_t *data, uint32_t len);

/**
 * @brief  一次性计算缓冲区 CRC32（结果与 Python `zlib.crc32(data) & 0xFFFFFFFF` 严格一致）
 */
uint32_t crc32_calc(const uint8_t *data, uint32_t len);

#endif /* __CRC32_H */
