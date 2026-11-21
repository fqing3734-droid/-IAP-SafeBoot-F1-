/**
 ******************************************************************************
 * @file    app_main.c
 * @brief   SafeBoot-F1 示例应用固件（0x08002000，支持编译出 v1.0 / v1.1 / v1.2_bad 三种镜像）
 * @note    SafeBoot-F1
 *
 * 编译宏 APP_VARIANT：
 *   10（默认）：App v1.0（0x00010000），正常运行，满 1000ms 调用 boot_meta_commit_app() 转正
 *   11       ：App v1.1（0x00010001），升级目标新版本，正常运行并转正
 *   99       ：App v1.2-BAD（0x00010002），故障注入固件：启动后模拟死锁，不喂狗也不 Commit，
 *              触发 IWDG 连续复位 3 次后由 Bootloader 自动回滚到上一版好固件！
 ******************************************************************************
 */
#include "bsp.h"
#include "bsp_flash.h"
#include "boot_meta.h"
#include "crc16.h"
#include "crc32.h"
#include "iap_proto.h"

#ifndef APP_VARIANT
#define APP_VARIANT             10
#endif

#if (APP_VARIANT == 11)
#define THIS_APP_VERSION        0x00010001u
#define LED_HALF_PERIOD_MS      125u
#elif (APP_VARIANT == 99)
#define THIS_APP_VERSION        0x00010002u
#define LED_HALF_PERIOD_MS      50u
#else
#define THIS_APP_VERSION        0x00010000u
#define LED_HALF_PERIOD_MS      500u
#endif

#define APP_COMMIT_DELAY_MS     1000u

static boot_meta_t s_meta;
static iap_ctx_t   s_iap;
#if (APP_VARIANT != 99)
static uint8_t     s_rsp[IAP_MAX_FRAME_LEN];
#endif

int main(void)
{
    uint8_t  from_bak  = 0u;
#if (APP_VARIANT != 99)
    uint8_t  committed = 0u;
    uint32_t boot_t0;
    uint8_t  ch;
    uint16_t rsp_len;
#endif

    /* 第一步：重定位中断向量表到 0x08002000（App 跑飞最常见根因就是漏掉这一句） */
    SCB->VTOR = SB_APP_ADDR;
    __DSB();
    __ISB();

    bsp_init();
    crc16_init();

    (void)boot_meta_load(&s_meta, &from_bak);
    s_meta.app_version = THIS_APP_VERSION;
    iap_proto_init(&s_iap, &s_meta, 1u);

#if (APP_VARIANT == 99)
    /*
     * 故障注入固件（阶段 5 破坏性实验）：
     * 故意死循环，既不调用 boot_meta_commit_app()，也不喂 IWDG。
     * 2 秒后硬件看门狗将芯片复位，连续 3 次后 Bootloader 自动回滚到旧版！
     */
    for (;;) {
        __NOP();
    }
#else
    boot_t0 = g_bsp_ms;

    for (;;) {
        /* 稳定运行满 1000ms 后向 Boot Meta 写入自我确认（STATE_TESTING -> STATE_NORMAL） */
        if (!committed && (uint32_t)(g_bsp_ms - boot_t0) >= APP_COMMIT_DELAY_MS) {
            if (s_meta.state == (uint16_t)BOOT_STATE_TESTING) {
                (void)boot_meta_commit_app();
                (void)boot_meta_load(&s_meta, &from_bak);
            }
            committed = 1u;
        }

        /* 监听串口：支持上位机随时查询运行状态（CMD_QUERY）或请求重启进 Bootloader 升级 */
        if (bsp_uart1_getc_nonblock(&ch)) {
            rsp_len = iap_proto_feed_byte(&s_iap, ch, s_rsp);
            if (rsp_len > 0u) {
                bsp_uart1_write(s_rsp, rsp_len);
                if (s_iap.reboot_requested) {
                    bsp_delay_ms(20u);
                    bsp_system_reset();
                }
            }
        }

        if (((g_bsp_ms / LED_HALF_PERIOD_MS) & 1u) == 0u) {
            bsp_led_on();
        } else {
            bsp_led_off();
        }

        bsp_iwdg_feed();
    }
#endif
}
