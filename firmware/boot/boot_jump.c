/**
 ******************************************************************************
 * @file    boot_jump.c
 * @brief   Cortex-M3 6 步安全跳转实现
 * @note    SafeBoot-F1
 ******************************************************************************
 */
#include "boot_jump.h"
#include "stm32f10x.h"

typedef void (*app_entry_t)(void);

uint8_t boot_check_vector_table(uint32_t app_addr)
{
    uint32_t sp  = *(volatile const uint32_t *)(app_addr + 0u);
    uint32_t rst = *(volatile const uint32_t *)(app_addr + 4u);

    /* 1. 栈顶必须落在 0x20000000 ~ 0x20005000（允许末地址等于 0x20005000） */
    if (sp < (SB_SRAM_BASE + 256u) || sp > SB_SRAM_END || (sp & 3u) != 0u) {
        return 0u;
    }
    /* 2. Reset_Handler 必须在 App 分区范围内，且 bit0 必须为 1（Cortex-M3 Thumb 状态） */
    if (rst < app_addr || rst >= (app_addr + SB_APP_SIZE) || (rst & 1u) == 0u) {
        return 0u;
    }
    return 1u;
}

#if !defined(HOST_SIM)
void boot_jump_to_app(uint32_t app_addr)
{
    uint32_t    sp  = *(volatile const uint32_t *)(app_addr + 0u);
    uint32_t    rst = *(volatile const uint32_t *)(app_addr + 4u);
    app_entry_t fn  = (app_entry_t)rst;

    if (!boot_check_vector_table(app_addr)) {
        return;
    }

    /* 第 1 步：关闭全局中断，防止清理外设与切 VTOR 期间进中断 */
    __disable_irq();

    /* 第 2 步：关闭 SysTick 定时器并清零计数器 */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;

    /* 第 3 步：硬件复位 Bootloader 用过的外设（USART1、TIM2、TIM3） */
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;
    RCC->APB1RSTR |= (RCC_APB1RSTR_TIM2RST | RCC_APB1RSTR_TIM3RST);
    RCC->APB1RSTR &= ~(RCC_APB1RSTR_TIM2RST | RCC_APB1RSTR_TIM3RST);

    /* 第 4 步：禁用并清除 NVIC 所有中断通道的使能位与挂起位 */
    NVIC->ICER[0] = 0xFFFFFFFFu;
    NVIC->ICER[1] = 0xFFFFFFFFu;
    NVIC->ICER[2] = 0xFFFFFFFFu;
    NVIC->ICPR[0] = 0xFFFFFFFFu;
    NVIC->ICPR[1] = 0xFFFFFFFFu;
    NVIC->ICPR[2] = 0xFFFFFFFFu;

    /* 第 5 步：预设 VTOR 指向 App 向量表，并切换 MSP 主栈指针 */
    SCB->VTOR = app_addr;
    __DSB();
    __ISB();
    __set_MSP(sp);

    /* 开启全局中断（App 的 startup_stm32f10x_md.s 与 SystemInit 在清洁环境中接管） */
    __enable_irq();

    /* 第 6 步：跳转到 App 的 Reset_Handler */
    fn();
}
#endif
