/**
 ******************************************************************************
 * @file    boot_jump.h
 * @brief   Cortex-M3 6 步安全跳转与 App 向量表头部合法性检查
 * @note    SafeBoot-F1
 ******************************************************************************
 */
#ifndef __BOOT_JUMP_H
#define __BOOT_JUMP_H

#include "common.h"

/**
 * @brief  检查目标地址处的 Cortex-M3 向量表首两字是否合法
 * @param  app_addr  App 分区起始地址（如 0x08002000）
 * @return 1 = 合法（MSP 在 20KB SRAM 内，Reset_Handler 为奇数 Thumb 地址且落在 App 区内）；0 = 非法
 */
uint8_t boot_check_vector_table(uint32_t app_addr);

/**
 * @brief  执行 6 步安全跳转到 App（不会返回）
 * @note   1. 校验栈顶与复位向量
 *         2. __disable_irq() 关全局中断
 *         3. 关闭 SysTick，通过 RCC RSTR 复位 USART1/TIM2/TIM3
 *         4. 清空 NVIC 全部 ICER 与 ICPR 挂起位
 *         5. 设置 SCB->VTOR = app_addr 并设置主栈指针 __set_MSP
 *         6. 跳入 App 的 Reset_Handler
 */
void    boot_jump_to_app(uint32_t app_addr);

#endif /* __BOOT_JUMP_H */
