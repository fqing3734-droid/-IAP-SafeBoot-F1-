/**
 ******************************************************************************
 * @file    bsp_flash.c
 * @brief   内部 Flash 擦写实现（直写 FPEC 寄存器）
 * @note    SafeBoot-F1
 ******************************************************************************
 */
#include "bsp_flash.h"
#include "crc32.h"
#include "stm32f10x.h"

#define BSP_FLASH_END           SB_FLASH_END
#define BSP_FLASH_KEY1          0x45670123u
#define BSP_FLASH_KEY2          0xCDEF89ABu

#define BSP_FLASH_SR_CLEAR      ((uint32_t)(FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR))
#define BSP_FLASH_SR_ERROR      ((uint32_t)(FLASH_SR_PGERR | FLASH_SR_WRPRTERR))
#define BSP_FLASH_POLL_LIMIT    0x00800000u

static uint8_t flash_wait_idle(void)
{
    uint32_t guard = BSP_FLASH_POLL_LIMIT;

    while ((FLASH->SR & FLASH_SR_BSY) != 0u) {
        if (--guard == 0u) {
            return 1u;
        }
    }
    return 0u;
}

static uint8_t flash_addr_ok(uint32_t addr, uint16_t len)
{
    if (addr < BSP_FLASH_WRITE_GUARD) {
        return 0u;  /* 严禁擦写 0x08002000 以下的 Bootloader 区 */
    }
    if (addr >= BSP_FLASH_END) {
        return 0u;
    }
    if (len != 0u && (uint32_t)len > (BSP_FLASH_END - addr)) {
        return 0u;
    }
    return 1u;
}

uint8_t bsp_flash_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) == 0u) {
        return BSP_FLASH_OK;
    }
    FLASH->KEYR = BSP_FLASH_KEY1;
    FLASH->KEYR = BSP_FLASH_KEY2;

    if ((FLASH->CR & FLASH_CR_LOCK) != 0u) {
        return BSP_FLASH_ERR_LOCK;
    }
    return BSP_FLASH_OK;
}

void bsp_flash_lock(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
}

uint8_t bsp_flash_erase_page(uint32_t addr)
{
    uint8_t rc;

    if (!flash_addr_ok(addr, 0u)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if ((addr & (BSP_FLASH_PAGE_SIZE - 1u)) != 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    if (bsp_flash_unlock() != BSP_FLASH_OK) {
        return BSP_FLASH_ERR_LOCK;
    }

    FLASH->SR = BSP_FLASH_SR_CLEAR;
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR  = addr;
    FLASH->CR |= FLASH_CR_STRT;

    rc = flash_wait_idle();
    FLASH->CR &= ~FLASH_CR_PER;

    if (rc != 0u) {
        bsp_flash_lock();
        return BSP_FLASH_ERR_ERASE;
    }
    if ((FLASH->SR & BSP_FLASH_SR_ERROR) != 0u) {
        FLASH->SR = BSP_FLASH_SR_CLEAR;
        bsp_flash_lock();
        return BSP_FLASH_ERR_ERASE;
    }
    FLASH->SR = BSP_FLASH_SR_CLEAR;
    bsp_flash_lock();
    return BSP_FLASH_OK;
}

uint8_t bsp_flash_program(uint32_t addr, const uint8_t *src, uint16_t len)
{
    uint16_t i;
    uint16_t hw;
    uint8_t  rc = BSP_FLASH_OK;

    if (src == 0 || len == 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    if ((addr & 1u) != 0u || (len & 1u) != 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    if (!flash_addr_ok(addr, len)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if ((addr / BSP_FLASH_PAGE_SIZE) != ((addr + (uint32_t)len - 1u) / BSP_FLASH_PAGE_SIZE)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if (bsp_flash_unlock() != BSP_FLASH_OK) {
        return BSP_FLASH_ERR_LOCK;
    }

    FLASH->SR = BSP_FLASH_SR_CLEAR;
    FLASH->CR |= FLASH_CR_PG;

    for (i = 0u; i < len; i += 2u) {
        hw = (uint16_t)((uint16_t)src[i] | ((uint16_t)src[i + 1u] << 8));
        *(volatile uint16_t *)(addr + (uint32_t)i) = hw;

        if (flash_wait_idle() != 0u) {
            rc = BSP_FLASH_ERR_PROG;
            break;
        }
        if ((FLASH->SR & BSP_FLASH_SR_ERROR) != 0u) {
            FLASH->SR = BSP_FLASH_SR_CLEAR;
            rc = BSP_FLASH_ERR_PROG;
            break;
        }
    }

    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->SR  = BSP_FLASH_SR_CLEAR;
    bsp_flash_lock();
    return rc;
}

void bsp_flash_read(uint32_t addr, uint8_t *dst, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)addr;
    uint16_t i;

    for (i = 0u; i < len; i++) {
        dst[i] = p[i];
    }
}

uint8_t bsp_flash_is_blank(uint32_t addr, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)addr;
    uint16_t i;

    for (i = 0u; i < len; i++) {
        if (p[i] != 0xFFu) {
            return 0u;
        }
    }
    return 1u;
}

uint32_t bsp_flash_crc32(uint32_t addr, uint32_t len)
{
    uint8_t  chunk[64];
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t rem = len;
    uint32_t cur = addr;
    uint16_t step;

    while (rem > 0u) {
        step = (uint16_t)((rem > sizeof(chunk)) ? sizeof(chunk) : rem);
        bsp_flash_read(cur, chunk, step);
        crc = crc32_update(crc, chunk, step);
        cur += step;
        rem -= step;
    }
    return crc ^ 0xFFFFFFFFu;
}
