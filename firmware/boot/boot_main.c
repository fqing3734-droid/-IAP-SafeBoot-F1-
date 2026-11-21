/**
 ******************************************************************************
 * @file    boot_main.c
 * @brief   SafeBoot-F1 Bootloader 主程序（0x08000000，8 KB）
 * @note    SafeBoot-F1
 *
 * 启动流程：
 *   1. 第一时间读取 RCC->CSR 复位源并清除 RMVF。
 *   2. 初始化时钟(72MHz)、串口、CRC16 表，加载双页 Boot Meta。
 *   3. 若当前处于 DOWNLOADING 状态，直接驻留 Bootloader 等待断点续传；
 *      否则开启 300ms 串口握手窗口：若收到上位机 IAP 帧则驻留升级，
 *      若超时未收到，则执行 boot_process_startup() 五态裁决（正常启动 / 逐页交换安装 / 3 次超时自动回滚）。
 *   4. 校验 App 向量表合法后，启动 IWDG(2s) 并执行 6 步安全跳转到 0x08002000。
 ******************************************************************************
 */
#include "bsp.h"
#include "bsp_flash.h"
#include "boot_jump.h"
#include "boot_meta.h"
#include "crc16.h"
#include "crc32.h"
#include "iap_proto.h"

#define BOOT_WAIT_WINDOW_MS     300u

static boot_meta_t s_meta;
static iap_ctx_t   s_iap;
static uint8_t     s_rsp[IAP_MAX_FRAME_LEN];

int main(void)
{
    uint32_t      rst_flags = bsp_reset_flags();
    uint8_t       from_bak  = 0u;
    uint8_t       stay_boot = 0u;
    uint32_t      t0;
    uint8_t       ch;
    uint16_t      rsp_len;
    boot_action_t act       = BOOT_ACT_STAY_BOOT;

    (void)rst_flags;
    bsp_reset_flags_clear();
    bsp_init();
    crc16_init();

    (void)boot_meta_load(&s_meta, &from_bak);
    iap_proto_init(&s_iap, &s_meta, 0u);

    /* 若正在下载中途掉电且原 App 空白，直接驻留等待续传 */
    if (s_meta.state == (uint16_t)BOOT_STATE_DOWNLOADING && s_meta.app_size == 0u) {
        stay_boot = 1u;
    }

    /* 300ms 串口升级握手窗口 */
    t0 = g_bsp_ms;
    while (!stay_boot && (uint32_t)(g_bsp_ms - t0) < BOOT_WAIT_WINDOW_MS) {
        if (bsp_uart1_getc_nonblock(&ch)) {
            rsp_len = iap_proto_feed_byte(&s_iap, ch, s_rsp);
            if (rsp_len > 0u) {
                bsp_uart1_write(s_rsp, rsp_len);
                stay_boot = 1u;
                break;
            }
        }
    }

    if (!stay_boot) {
        if (boot_process_startup(&s_meta, &act) == BOOT_OK &&
            boot_check_vector_table(SB_APP_ADDR)) {
            /* 跳转前开启 2s 独立看门狗：若新固件死锁未 Commit，3 次复位后自动回滚 */
            bsp_iwdg_init();
            boot_jump_to_app(SB_APP_ADDR);
        }
        /* 若无有效 App 或向量表非法，自动留在 Bootloader 等待串口烧写 */
    }

    /* 驻留 Bootloader IAP 升级循环 */
    for (;;) {
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
        if (((g_bsp_ms / 150u) & 1u) == 0u) {
            bsp_led_on();
        } else {
            bsp_led_off();
        }
        bsp_iwdg_feed();
    }
}
