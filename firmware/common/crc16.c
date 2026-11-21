/**
 ******************************************************************************
 * @file    crc16.c
 * @brief   Modbus RTU CRC-16 实现（查表法，表在启动时生成）
 *
 * 多项式：0xA001（0x8005 的位反转形式，因为是右移算法）
 * 初值  ：0xFFFF
 * 结果  ：不取反
 * 发送  ：低字节在前
 ******************************************************************************
 */
#include "crc16.h"

/* 512 字节 RAM，换取每字节约 10 个周期的校验速度 */
static uint16_t s_crc_table[256];

void crc16_init(void)
{
    uint16_t i;
    uint8_t  bit;
    uint16_t c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (bit = 0; bit < 8; bit++) {
            if (c & 0x0001u) {
                c = (uint16_t)((c >> 1) ^ 0xA001u);
            } else {
                c = (uint16_t)(c >> 1);
            }
        }
        s_crc_table[i] = c;
    }
}

uint16_t crc16_modbus(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;

    while (len--) {
        crc = (uint16_t)((crc >> 8) ^ s_crc_table[(crc ^ *data) & 0x00FFu]);
        data++;
    }
    return crc;
}
