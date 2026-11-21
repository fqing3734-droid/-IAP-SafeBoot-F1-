/**
 ******************************************************************************
 * @file    bsp.h
 * @brief   SafeBoot-F1 板级支持包：72MHz 时钟、SysTick、USART1 环形缓冲收发、IWDG、软复位
 * @note    目标芯片 STM32F103C8T6 / GD32F103C8T6
 ******************************************************************************
 */
#ifndef __BSP_H
#define __BSP_H

#include "common.h"
#include "stm32f10x.h"

#define BSP_UART_BAUD           9600u
#define BSP_LED_PORT            GPIOC
#define BSP_LED_PIN             13u
#define BSP_DE_PORT             GPIOA
#define BSP_DE_PIN              8u

#define BSP_IWDG_ENABLE         1
#define BSP_IWDG_TIMEOUT_MS     2000u

extern volatile uint32_t g_bsp_ms;

void        bsp_init(void);
void        bsp_delay_ms(uint32_t ms);

void        bsp_led_on(void);
void        bsp_led_off(void);
void        bsp_led_toggle(void);

void        bsp_rs485_tx(uint8_t on);
void        bsp_uart1_putc(char c);
void        bsp_uart1_write(const uint8_t *p, uint16_t n);
uint8_t     bsp_uart1_getc_nonblock(uint8_t *out);
void        con_printf(const char *fmt, ...);

void        bsp_iwdg_init(void);
void        bsp_iwdg_feed(void);
uint32_t    bsp_reset_flags(void);
void        bsp_reset_flags_clear(void);
const char *bsp_reset_cause_name(uint32_t flags);
void        bsp_system_reset(void);

#endif /* __BSP_H */
