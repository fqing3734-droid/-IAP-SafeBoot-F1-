/**
 ******************************************************************************
 * @file    crc32.c
 * @brief   IEEE 802.3 CRC-32 实现（半字节查表法：仅占 64 字节 Flash，零 RAM 占用）
 * @note    SafeBoot-F1
 *
 * 为什么用半字节（Nibble，16 项）查表而不是 256 项全表：
 *   256 项全表占 1024 字节（整整 1 页 Flash）；
 *   半字节表只占 16 × 4 = 64 字节，每字节查表两次，算完 24KB 固件在 72MHz 下
 *   不到 4 ms，对 Bootloader 来说省下 960 字节代码空间划算得多。
 ******************************************************************************
 */
#include "crc32.h"

static const uint32_t s_crc32_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
    0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
    0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint8_t  b;

    for (i = 0u; i < len; i++) {
        b = data[i];
        crc = (crc >> 4) ^ s_crc32_nibble[(crc ^ (b >> 0)) & 0x0Fu];
        crc = (crc >> 4) ^ s_crc32_nibble[(crc ^ (b >> 4)) & 0x0Fu];
    }
    return crc;
}

uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}
