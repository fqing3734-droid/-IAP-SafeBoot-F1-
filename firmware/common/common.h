/**
 ******************************************************************************
 * @file    common.h
 * @brief   SafeBoot-F1 全工程公共定义（类型、断言、64KB Flash 四区划分）
 * @note    SafeBoot-F1  |  面向工业现场的防变砖 IAP 固件升级系统
 *
 * 64KB 片内 Flash 布局（STM32F103C8T6 / GD32F103C8T6，1 页 = 1 KB）：
 *   0x0800 0000 ~ 0x0800 1FFF   Bootloader 区    (8 KB,  Page 0~7)
 *   0x0800 2000 ~ 0x0800 7FFF   App 运行区       (24 KB, Page 8~31)
 *   0x0800 8000 ~ 0x0800 DFFF   Slot 备份/下载区 (24 KB, Page 32~55)
 *   0x0800 E000 ~ 0x0800 E7FF   Boot Meta 双页   (2 KB,  Page 56~57)
 *   0x0800 E800 ~ 0x0800 F7FF   预留区           (4 KB,  Page 58~61)
 *   0x0800 F800 ~ 0x0800 FFFF   应用参数双区     (2 KB,  Page 62~63，兼容 RIO-4AI2DO)
 ******************************************************************************
 */
#ifndef __COMMON_H
#define __COMMON_H

#include <stdint.h>

/* ---- 编译期断言 ---- */
#define STATIC_ASSERT(cond, msg)    typedef char static_assert_##msg[(cond) ? 1 : -1]

/* ---- 位操作与工具 ---- */
#define BIT(n)                      (1UL << (n))
#define ARRAY_SIZE(a)               (sizeof(a) / sizeof((a)[0]))
#define MIN_OF(a, b)                (((a) < (b)) ? (a) : (b))
#define MAX_OF(a, b)                (((a) > (b)) ? (a) : (b))

/* ======================= 64KB Flash 分区表 ======================= */

#define SB_FLASH_BASE               0x08000000u
#define SB_FLASH_PAGE_SIZE          1024u
#define SB_FLASH_TOTAL_SIZE         (64u * 1024u)
#define SB_FLASH_END                (SB_FLASH_BASE + SB_FLASH_TOTAL_SIZE)

/* 1. Bootloader 区：8 KB（Page 0~7） */
#define SB_BOOT_ADDR                0x08000000u
#define SB_BOOT_SIZE                (8u * 1024u)

/* 2. App 运行区：24 KB（Page 8~31） */
#define SB_APP_ADDR                 0x08002000u
#define SB_APP_SIZE                 (24u * 1024u)
#define SB_APP_PAGES                24u

/* 3. Slot 下载与回滚备份区：24 KB（Page 32~55） */
#define SB_SLOT_ADDR                0x08008000u
#define SB_SLOT_SIZE                (24u * 1024u)
#define SB_SLOT_PAGES               24u

/* 4. Boot Meta 双页状态区：2 KB（Page 56 主页，Page 57 备页） */
#define SB_META_MAIN_ADDR           0x0800E000u
#define SB_META_BACK_ADDR           0x0800E400u
#define SB_META_PAGE_SIZE           1024u

/* 5. Golden 已知好固件暂存区（复用预留区 4 KB，或 Slot 搬运前互换） */
#define SB_RSVD_ADDR                0x0800E800u
#define SB_RSVD_SIZE                (4u * 1024u)

/* 6. 应用参数双区：与 RIO-4AI2DO 保持一致 */
#define SB_PARAM_MAIN_ADDR          0x0800F800u
#define SB_PARAM_BACK_ADDR          0x0800FC00u

/* Bootloader 写保护线：严禁任何代码擦写 0x08002000 以下的 Bootloader 自身 */
#define SB_FLASH_WRITE_GUARD        SB_APP_ADDR

/* STM32F103C8 20KB SRAM 范围（用于校验 App 向量表首字 MSP 合法性） */
#define SB_SRAM_BASE                0x20000000u
#define SB_SRAM_SIZE                (20u * 1024u)
#define SB_SRAM_END                 (SB_SRAM_BASE + SB_SRAM_SIZE)

#endif /* __COMMON_H */
