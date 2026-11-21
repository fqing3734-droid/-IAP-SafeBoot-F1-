/**
 ******************************************************************************
 * @file    crc16.h
 * @brief   Modbus RTU 使用的 CRC-16/MODBUS 校验
 ******************************************************************************
 */
#ifndef __CRC16_H
#define __CRC16_H

#include "common.h"

/**
 * @brief  初始化 CRC 查找表（必须在 main 里、其它 CRC 调用之前调用一次）
 * @note   用查表法而不是逐位法：查表约 1 次移位 + 1 次查表 ≈ 10 周期/字节，
 *         逐位法要 8 次循环 ≈ 100 周期/字节。Modbus 一帧最多 256 字节，
 *         在 115200bps 下留给我们的响应时间很紧，这点差距是值得的。
 *         表在 RAM 里现场生成（512 字节），比在 Flash 里手抄 256 个常量安全
 *         ——手抄错一个值，表现为"偶尔某一帧 CRC 不过"，极难查。
 */
void crc16_init(void);

/**
 * @brief  计算 Modbus CRC16
 * @param  data  数据指针
 * @param  len   字节数
 * @return CRC 值（低字节先发）
 * @note   Modbus 的规定：先发低字节，再发高字节。
 *         所以发送时是 data[n] = crc & 0xFF; data[n+1] = crc >> 8;
 */
uint16_t crc16_modbus(const uint8_t *data, uint16_t len);

#endif /* __CRC16_H */
