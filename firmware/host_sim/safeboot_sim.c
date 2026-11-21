/**
 ******************************************************************************
 * @file    safeboot_sim.c
 * @brief   SafeBoot-F1 PC 端 SIL（Software-in-the-Loop）虚拟 STM32F103 仿真器
 * @note    直接链接真实 firmware/middleware/boot_meta.c、iap_proto.c、crc16.c、crc32.c，
 *          通过标准输入/输出二进制管道模拟 USART1，通过 64KB 磁盘镜像模拟片内 Flash：
 *            - 严格复现 Bootloader 300ms 串口握手窗口
 *            - 严格复现 64KB Flash 写保护线与 NOR 物理特性（每次擦写落盘持久化）
 *            - 严格复现 App 向量表校验、健康固件 Commit 转正与坏固件 3 次 IWDG 复位自动回滚
 *
 *          用法：
 *            safeboot_sim.exe [--flash sim_flash.bin] [--boot-wait-ms 300] [--stay-boot]
 *
 *          SIL 测试固件可选行为标签（位于镜像偏移 0x0100）：
 *            [0..3]  magic   = 0x50414253 ("SBAP")
 *            [4..7]  version = uint32
 *            [8..11] flags   = bit0(1): 模拟死锁不喂狗不 Commit（等效 APP_VARIANT=99）
 ******************************************************************************
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#include "common.h"
#include "crc16.h"
#include "crc32.h"
#include "bsp_flash.h"
#include "boot_meta.h"
#include "iap_proto.h"

#define SIM_APP_TAG_OFFSET      0x0100u
#define SIM_APP_TAG_MAGIC       0x50414253u  /* "SBAP" */
#define SIM_APP_FLAG_DEADLOCK   0x00000001u

/* ======================= 64KB 持久化虚拟 Flash ======================= */

static uint8_t     g_flash[SB_FLASH_TOTAL_SIZE];
static const char *g_flash_path = NULL;

static void flash_file_load(void)
{
    FILE *fp;

    memset(g_flash, 0xFF, sizeof(g_flash));
    if (g_flash_path == NULL) {
        return;
    }
    fp = fopen(g_flash_path, "rb");
    if (fp != NULL) {
        size_t n = fread(g_flash, 1u, sizeof(g_flash), fp);
        (void)n;
        fclose(fp);
    }
}

static void flash_file_flush(void)
{
    FILE *fp;

    if (g_flash_path == NULL) {
        return;
    }
    fp = fopen(g_flash_path, "wb");
    if (fp != NULL) {
        (void)fwrite(g_flash, 1u, sizeof(g_flash), fp);
        fclose(fp);
    }
}

static uint8_t sim_addr_ok(uint32_t addr, uint16_t len)
{
    if (addr < SB_FLASH_WRITE_GUARD || addr >= SB_FLASH_END) {
        return 0u;
    }
    if (len != 0u && (uint32_t)len > (SB_FLASH_END - addr)) {
        return 0u;
    }
    return 1u;
}

uint8_t bsp_flash_unlock(void) { return BSP_FLASH_OK; }
void    bsp_flash_lock(void)   { }

uint8_t bsp_flash_erase_page(uint32_t addr)
{
    uint32_t off;

    if (!sim_addr_ok(addr, 0u)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if ((addr & (SB_FLASH_PAGE_SIZE - 1u)) != 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    off = addr - SB_FLASH_BASE;
    memset(&g_flash[off], 0xFF, SB_FLASH_PAGE_SIZE);
    flash_file_flush();
    return BSP_FLASH_OK;
}

uint8_t bsp_flash_program(uint32_t addr, const uint8_t *src, uint16_t len)
{
    uint32_t off;
    uint16_t i;

    if (src == NULL || len == 0u || (addr & 1u) != 0u || (len & 1u) != 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    if (!sim_addr_ok(addr, len)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if ((addr / SB_FLASH_PAGE_SIZE) != ((addr + (uint32_t)len - 1u) / SB_FLASH_PAGE_SIZE)) {
        return BSP_FLASH_ERR_RANGE;
    }

    off = addr - SB_FLASH_BASE;
    for (i = 0u; i < len; i++) {
        if ((src[i] & (uint8_t)(~g_flash[off + i])) != 0u) {
            return BSP_FLASH_ERR_PROG;
        }
        g_flash[off + i] &= src[i];
    }
    flash_file_flush();
    return BSP_FLASH_OK;
}

void bsp_flash_read(uint32_t addr, uint8_t *dst, uint16_t len)
{
    uint32_t off;

    if (addr < SB_FLASH_BASE || addr >= SB_FLASH_END) {
        memset(dst, 0xFF, len);
        return;
    }
    off = addr - SB_FLASH_BASE;
    if ((uint32_t)len > SB_FLASH_TOTAL_SIZE - off) {
        len = (uint16_t)(SB_FLASH_TOTAL_SIZE - off);
    }
    memcpy(dst, &g_flash[off], len);
}

uint8_t bsp_flash_is_blank(uint32_t addr, uint16_t len)
{
    uint32_t off = addr - SB_FLASH_BASE;
    uint16_t i;

    for (i = 0u; i < len; i++) {
        if (g_flash[off + i] != 0xFFu) {
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

/* ======================= Cortex-M3 向量表与 App 行为仿真 ======================= */

static uint32_t read_u32_flash(uint32_t addr)
{
    uint8_t b[4];
    bsp_flash_read(addr, b, 4u);
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static uint8_t sim_check_vector_table(uint32_t app_addr)
{
    uint32_t sp  = read_u32_flash(app_addr + 0u);
    uint32_t rst = read_u32_flash(app_addr + 4u);

    if (sp < (SB_SRAM_BASE + 256u) || sp > SB_SRAM_END || (sp & 3u) != 0u) {
        return 0u;
    }
    if (rst < app_addr || rst >= (app_addr + SB_APP_SIZE) || (rst & 1u) == 0u) {
        return 0u;
    }
    return 1u;
}

static uint32_t sim_app_flags(uint32_t app_addr)
{
    uint32_t magic = read_u32_flash(app_addr + SIM_APP_TAG_OFFSET);
    if (magic == SIM_APP_TAG_MAGIC) {
        return read_u32_flash(app_addr + SIM_APP_TAG_OFFSET + 8u);
    }
    return 0u;
}

/* ======================= 二进制串口管道 I/O ======================= */

static HANDLE g_hIn  = INVALID_HANDLE_VALUE;
static HANDLE g_hOut = INVALID_HANDLE_VALUE;

static void uart_tx(const uint8_t *buf, uint16_t len)
{
    DWORD written = 0u;
    uint16_t sent = 0u;

    while (sent < len) {
        if (!WriteFile(g_hOut, buf + sent, (DWORD)(len - sent), &written, NULL)) {
            break;
        }
        sent = (uint16_t)(sent + written);
    }
}

/* 返回值：1=读到 1 字节，0=超时无字节，-1=管道断开(EOF) */
static int uart_rx_byte_timeout(uint8_t *out, uint32_t timeout_ms)
{
    DWORD t0 = GetTickCount();

    for (;;) {
        DWORD avail = 0u;
        if (!PeekNamedPipe(g_hIn, NULL, 0u, NULL, &avail, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                return -1;
            }
            /* 非管道（如控制台/文件）退化为阻塞读 */
            DWORD nread = 0u;
            if (!ReadFile(g_hIn, out, 1u, &nread, NULL) || nread == 0u) {
                return -1;
            }
            return 1;
        }
        if (avail > 0u) {
            DWORD nread = 0u;
            if (!ReadFile(g_hIn, out, 1u, &nread, NULL) || nread == 0u) {
                return -1;
            }
            return 1;
        }
        if ((GetTickCount() - t0) >= timeout_ms) {
            return 0;
        }
        Sleep(5u);
    }
}

static int uart_rx_byte_blocking(uint8_t *out)
{
    DWORD nread = 0u;
    if (!ReadFile(g_hIn, out, 1u, &nread, NULL) || nread == 0u) {
        return -1;
    }
    return 1;
}

/* ======================= 虚拟 MCU 启动状态机 ======================= */

static boot_meta_t s_meta;
static iap_ctx_t   s_iap;
static uint8_t     s_rsp[IAP_MAX_FRAME_LEN];
static uint32_t    g_boot_wait_ms = 300u;

static const char *state_name(uint16_t st)
{
    switch (st) {
    case BOOT_STATE_NORMAL:          return "NORMAL";
    case BOOT_STATE_DOWNLOADING:     return "DOWNLOADING";
    case BOOT_STATE_PENDING_INSTALL: return "PENDING_INSTALL";
    case BOOT_STATE_TESTING:         return "TESTING";
    case BOOT_STATE_ROLLBACK:        return "ROLLBACK";
    default:                         return "UNKNOWN";
    }
}

/**
 * @brief  模拟一次 MCU 上电 / 软复位 / IWDG 复位流程
 * @param  wait_ms   Bootloader 握手窗口时长（ms）
 * @param  force_boot 1 = 强制驻留 Bootloader
 * @return 0 = 正常进入串口循环，-1 = 宿主机已断开管道
 */
static int sim_mcu_boot(uint32_t wait_ms, uint8_t force_boot)
{
    int guard;

    for (guard = 0; guard < 16; guard++) {
        uint8_t       from_bak  = 0u;
        uint8_t       stay_boot = force_boot;
        boot_action_t act       = BOOT_ACT_STAY_BOOT;
        uint8_t       rc;

        force_boot = 0u;
        (void)boot_meta_load(&s_meta, &from_bak);
        iap_proto_init(&s_iap, &s_meta, 0u);

        fprintf(stderr, "[SIM] RESET -> state=%s try=%u app=0x%08X(%uB) slot=0x%08X(%uB) dl_seq=%u\n",
                state_name(s_meta.state),
                (unsigned)s_meta.try_cnt,
                (unsigned)s_meta.app_version, (unsigned)s_meta.app_size,
                (unsigned)s_meta.slot_version, (unsigned)s_meta.slot_size,
                (unsigned)s_meta.dl_done_seq);
        fflush(stderr);

        if (s_meta.state == (uint16_t)BOOT_STATE_DOWNLOADING && s_meta.app_size == 0u) {
            stay_boot = 1u;
        }

        /* 模拟 Bootloader 300ms 串口握手窗口 */
        if (!stay_boot && wait_ms > 0u) {
            DWORD t0 = GetTickCount();
            while ((GetTickCount() - t0) < wait_ms) {
                uint32_t rem = wait_ms - (GetTickCount() - t0);
                uint8_t  ch  = 0u;
                int      r   = uart_rx_byte_timeout(&ch, rem);
                if (r < 0) {
                    return -1;
                }
                if (r > 0) {
                    uint16_t rsp_len = iap_proto_feed_byte(&s_iap, ch, s_rsp);
                    if (rsp_len > 0u) {
                        uart_tx(s_rsp, rsp_len);
                        stay_boot = 1u;
                        fprintf(stderr, "[SIM] Handshake caught -> stay in BOOTLOADER\n");
                        fflush(stderr);
                        if (s_iap.reboot_requested) {
                            /* 握手窗口里就收到了 REBOOT，直接再重启 */
                            stay_boot = 0u;
                            goto do_next_reset;
                        }
                        break;
                    }
                }
            }
        }

        if (stay_boot) {
            fprintf(stderr, "[SIM] Running in BOOTLOADER (run_mode=0)\n");
            fflush(stderr);
            return 0;
        }

        /* 窗口超时未收到握手 -> 执行真实 boot_process_startup() 裁决 */
        rc = boot_process_startup(&s_meta, &act);
        fprintf(stderr, "[SIM] boot_process_startup rc=%u act=%u -> state=%s app=0x%08X try=%u evt=%u\n",
                (unsigned)rc, (unsigned)act, state_name(s_meta.state),
                (unsigned)s_meta.app_version, (unsigned)s_meta.try_cnt, (unsigned)s_meta.last_event);
        fflush(stderr);

        if (rc == BOOT_OK && sim_check_vector_table(SB_APP_ADDR)) {
            uint32_t flags = sim_app_flags(SB_APP_ADDR);
            if ((flags & SIM_APP_FLAG_DEADLOCK) != 0u) {
                /* 模拟故障固件死循环 -> 2s 后硬件 IWDG 复位 -> 不经过握手等待直接再次重启 */
                fprintf(stderr, "[SIM] App 0x%08X DEADLOCKED! IWDG timeout -> hard reset...\n",
                        (unsigned)s_meta.app_version);
                fflush(stderr);
                wait_ms = 0u;
                continue;
            }

            /* 健康 App：若处于 TESTING 则自动调用 boot_meta_commit_app() 转正 */
            if (s_meta.state == (uint16_t)BOOT_STATE_TESTING) {
                (void)boot_meta_commit_app();
                (void)boot_meta_load(&s_meta, &from_bak);
                fprintf(stderr, "[SIM] App 0x%08X COMMITTED -> state=%s evt=%u\n",
                        (unsigned)s_meta.app_version, state_name(s_meta.state), (unsigned)s_meta.last_event);
                fflush(stderr);
            }

            iap_proto_init(&s_iap, &s_meta, 1u);
            fprintf(stderr, "[SIM] Running in APP 0x%08X (run_mode=1)\n", (unsigned)s_meta.app_version);
            fflush(stderr);
            return 0;
        }

        /* 无有效 App 或向量表非法 -> 留在 Bootloader */
        iap_proto_init(&s_iap, &s_meta, 0u);
        fprintf(stderr, "[SIM] No valid App -> stay in BOOTLOADER (run_mode=0)\n");
        fflush(stderr);
        return 0;

    do_next_reset:
        wait_ms = g_boot_wait_ms;
    }

    fprintf(stderr, "[SIM] Reset guard reached, staying in BOOTLOADER\n");
    fflush(stderr);
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t force_boot = 0u;
    int     i;

#ifdef _WIN32
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    g_hIn  = GetStdHandle(STD_INPUT_HANDLE);
    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
#endif

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--flash") == 0 && (i + 1) < argc) {
            g_flash_path = argv[++i];
        } else if (strcmp(argv[i], "--boot-wait-ms") == 0 && (i + 1) < argc) {
            g_boot_wait_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stay-boot") == 0) {
            force_boot = 1u;
        }
    }

    crc16_init();
    flash_file_load();

    if (sim_mcu_boot(g_boot_wait_ms, force_boot) < 0) {
        return 0;
    }

    for (;;) {
        uint8_t  ch = 0u;
        uint16_t rsp_len;

        if (uart_rx_byte_blocking(&ch) < 0) {
            break;
        }
        rsp_len = iap_proto_feed_byte(&s_iap, ch, s_rsp);
        if (rsp_len > 0u) {
            uart_tx(s_rsp, rsp_len);
            if (s_iap.reboot_requested) {
                /*
                 * 若因 CMD_END 触发重启（PENDING_INSTALL），直接执行搬运与试运行闭环；
                 * 若因 CMD_REBOOT 触发重启（通常从 App 切回 Boot），开启握手窗口等待上位机接管。
                 */
                uint32_t next_wait = (s_meta.state == (uint16_t)BOOT_STATE_PENDING_INSTALL)
                                   ? 0u
                                   : g_boot_wait_ms;
                if (sim_mcu_boot(next_wait, 0u) < 0) {
                    break;
                }
            }
        }
    }

    return 0;
}
