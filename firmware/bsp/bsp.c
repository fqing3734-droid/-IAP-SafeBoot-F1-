/**
 ******************************************************************************
 * @file    bsp.c
 * @brief   SafeBoot-F1 板级支持包实现（带 256B 中断接收环形缓冲 + TC 防截断发送）
 * @note    SafeBoot-F1
 ******************************************************************************
 */
#include "bsp.h"
#include <stdarg.h>

volatile uint32_t g_bsp_ms = 0u;

/* 256 字节中断接收环形缓冲（uint8 索引天然 0~255 回绕，无需模运算） */
static volatile uint8_t s_rx_ring[256];
static volatile uint8_t s_rx_head = 0u;
static volatile uint8_t s_rx_tail = 0u;

static void clock_init_72mhz(void)
{
    uint32_t timeout;

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) { }

    RCC->CR &= ~RCC_CR_PLLON;
    while (RCC->CR & RCC_CR_PLLRDY) { }

    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;

    RCC->CR |= RCC_CR_HSEON;
    timeout = 0x50000u;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
        if (--timeout == 0u) {
            return;
        }
    }

    RCC->CFGR = RCC_CFGR_HPRE_DIV1
              | RCC_CFGR_PPRE1_DIV2
              | RCC_CFGR_PPRE2_DIV1
              | RCC_CFGR_PLLSRC_HSE
              | RCC_CFGR_PLLMULL9;

    RCC->CR |= RCC_CR_PLLON;
    timeout = 0x50000u;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
        if (--timeout == 0u) {
            return;
        }
    }

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }
}

static void gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN;

    /* PA8 = RS485 DE 推挽输出，PA9 = USART1_TX 复用推挽，PA10 = USART1_RX 浮空输入 */
    GPIOA->CRH = 0x444444B2u;
    GPIOA->BRR = BIT(BSP_DE_PIN);

    /* PC13 = 板载 LED（低电平点亮） */
    GPIOC->CRH  = 0x44244444u;
    GPIOC->BSRR = BIT(BSP_LED_PIN);
}

static void usart1_init(uint32_t baud)
{
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    USART1->BRR = (uint16_t)((72000000u + (baud / 2u)) / baud);
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;

    NVIC_SetPriority(USART1_IRQn, 0u);
    NVIC_EnableIRQ(USART1_IRQn);
}

void SysTick_Handler(void)
{
    g_bsp_ms++;
}

void USART1_IRQHandler(void)
{
    uint16_t sr = USART1->SR;

    if (sr & USART_SR_RXNE) {
        uint8_t b = (uint8_t)(USART1->DR & 0xFFu);
        uint8_t next = (uint8_t)(s_rx_head + 1u);
        if (next != s_rx_tail) {
            s_rx_ring[s_rx_head] = b;
            s_rx_head = next;
        }
    }
    if (sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE)) {
        (void)USART1->DR;
    }
}

void bsp_init(void)
{
    s_rx_head = 0u;
    s_rx_tail = 0u;
    clock_init_72mhz();
    gpio_init();
    usart1_init(BSP_UART_BAUD);
    SysTick_Config(72000000u / 1000u);
}

void bsp_delay_ms(uint32_t ms)
{
    uint32_t t0 = g_bsp_ms;
    while ((uint32_t)(g_bsp_ms - t0) < ms) { }
}

void bsp_led_on(void)
{
    GPIOC->BRR = BIT(BSP_LED_PIN);
}

void bsp_led_off(void)
{
    GPIOC->BSRR = BIT(BSP_LED_PIN);
}

void bsp_led_toggle(void)
{
    if (GPIOC->ODR & BIT(BSP_LED_PIN)) {
        bsp_led_on();
    } else {
        bsp_led_off();
    }
}

void bsp_rs485_tx(uint8_t on)
{
    if (on) {
        GPIOA->BSRR = BIT(BSP_DE_PIN);
    } else {
        GPIOA->BRR = BIT(BSP_DE_PIN);
    }
}

void bsp_uart1_putc(char c)
{
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = (uint8_t)c;
}

void bsp_uart1_write(const uint8_t *p, uint16_t n)
{
    bsp_rs485_tx(1u);
    while (n--) {
        while (!(USART1->SR & USART_SR_TXE)) { }
        USART1->DR = *p++;
    }
    /* 等移位寄存器彻底发完（TC）再拉低 DE，避免末字节停止位截断 */
    while (!(USART1->SR & USART_SR_TC)) { }
    bsp_rs485_tx(0u);
}

uint8_t bsp_uart1_getc_nonblock(uint8_t *out)
{
    if (s_rx_head == s_rx_tail) {
        return 0u;
    }
    *out = s_rx_ring[s_rx_tail];
    s_rx_tail = (uint8_t)(s_rx_tail + 1u);
    return 1u;
}

static void con_put_uint(uint32_t v, uint8_t base, uint8_t width, char pad)
{
    char    buf[12];
    uint8_t i = 0u;

    if (v == 0u) {
        buf[i++] = '0';
    } else {
        while (v > 0u) {
            uint8_t d = (uint8_t)(v % base);
            buf[i++] = (char)((d < 10u) ? ('0' + d) : ('A' + d - 10u));
            v /= base;
        }
    }
    while (i < width) {
        buf[i++] = pad;
    }
    while (i > 0u) {
        bsp_uart1_putc(buf[--i]);
    }
}

void con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    while (*fmt) {
        if (*fmt != '%') {
            bsp_uart1_putc(*fmt++);
            continue;
        }
        fmt++;
        char    pad = ' ';
        uint8_t width = 0u;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = (uint8_t)(width * 10u + (uint8_t)(*fmt - '0'));
            fmt++;
        }
        switch (*fmt) {
        case 'u':
            con_put_uint(va_arg(ap, uint32_t), 10u, width, pad);
            break;
        case 'd':
        case 'i': {
            int32_t val = va_arg(ap, int32_t);
            if (val < 0) {
                bsp_uart1_putc('-');
                val = -val;
            }
            con_put_uint((uint32_t)val, 10u, width, pad);
            break;
        }
        case 'x':
        case 'X':
            con_put_uint(va_arg(ap, uint32_t), 16u, width, pad);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) { s = "(null)"; }
            while (*s) { bsp_uart1_putc(*s++); }
            break;
        }
        case 'c':
            bsp_uart1_putc((char)va_arg(ap, int));
            break;
        case '%':
            bsp_uart1_putc('%');
            break;
        default:
            bsp_uart1_putc(*fmt);
            break;
        }
        if (*fmt) { fmt++; }
    }
    va_end(ap);
}

void bsp_iwdg_init(void)
{
#if BSP_IWDG_ENABLE
    uint32_t reload = ((uint32_t)BSP_IWDG_TIMEOUT_MS * 625u) / 1000u;
    if (reload > 4095u) { reload = 4095u; }

    IWDG->KR  = 0xCCCCu;
    IWDG->KR  = 0x5555u;
    IWDG->PR  = 0x04u;
    IWDG->RLR = (uint16_t)reload;
    while (IWDG->SR != 0u) { }
    IWDG->KR  = 0xAAAAu;
#endif
}

void bsp_iwdg_feed(void)
{
#if BSP_IWDG_ENABLE
    IWDG->KR = 0xAAAAu;
#endif
}

uint32_t bsp_reset_flags(void)
{
    return RCC->CSR;
}

void bsp_reset_flags_clear(void)
{
    RCC->CSR |= RCC_CSR_RMVF;
    (void)RCC->CSR;
}

const char *bsp_reset_cause_name(uint32_t flags)
{
    if (flags & RCC_CSR_IWDGRSTF) { return "IWDG"; }
    if (flags & RCC_CSR_SFTRSTF)  { return "SFT";  }
    if (flags & RCC_CSR_PORRSTF)  { return "POR";  }
    if (flags & RCC_CSR_PINRSTF)  { return "PIN";  }
    return "?";
}

void bsp_system_reset(void)
{
    NVIC_SystemReset();
}
