/**
 ******************************************************************************
 * @file    bsp_flash.h
 * @brief   SafeBoot-F1 内部 Flash 擦写接口（直写 FPEC 寄存器 + 保护 Bootloader 区）
 * @note    保护线 BSP_FLASH_WRITE_GUARD = 0x08002000（严禁擦写 Bootloader 自身）
 ******************************************************************************
 */
#ifndef __BSP_FLASH_H
#define __BSP_FLASH_H

#include "common.h"

#define BSP_FLASH_OK            0u
#define BSP_FLASH_ERR_LOCK      1u
#define BSP_FLASH_ERR_ERASE     2u
#define BSP_FLASH_ERR_PROG      3u
#define BSP_FLASH_ERR_RANGE     4u
#define BSP_FLASH_ERR_ALIGN     5u

#define BSP_FLASH_BASE          SB_FLASH_BASE
#define BSP_FLASH_PAGE_SIZE     SB_FLASH_PAGE_SIZE
#define BSP_FLASH_WRITE_GUARD   SB_FLASH_WRITE_GUARD

uint8_t bsp_flash_unlock(void);
void    bsp_flash_lock(void);
uint8_t bsp_flash_erase_page(uint32_t addr);
uint8_t bsp_flash_program(uint32_t addr, const uint8_t *src, uint16_t len);
void    bsp_flash_read(uint32_t addr, uint8_t *dst, uint16_t len);
uint8_t bsp_flash_is_blank(uint32_t addr, uint16_t len);

/**
 * @brief  对 Flash 指定区间直接计算 CRC32（无需整块读入 RAM）
 */
uint32_t bsp_flash_crc32(uint32_t addr, uint32_t len);

#endif /* __BSP_FLASH_H */
