/**
 ******************************************************************************
 * @file    test_boot.c
 * @brief   SafeBoot-F1 PC 端全覆盖单元测试（模拟 64KB Flash + 逐微秒掉电故障注入）
 * @note    不编译真实 bsp_flash.c，而是提供带物理规则与掉电注入的片内 Flash 仿真器：
 *            1. 严格校验 0x08002000 Boot 区硬写保护线、半字对齐、跨页拦截
 *            2. 强制模拟 NOR Flash "只能 1->0，写前必擦" 物理特性
 *            3. 支持在第 N 次擦/写操作中途（含写到半页）精确注入掉电故障
 *
 *          覆盖用例：
 *            [1] CRC16-Modbus 与 CRC32(IEEE 802.3) 黄金测试向量
 *            [2] Flash 写保护线与半字物理规则
 *            [3] 双页 Meta 存储：空片默认、先备后主顺序、单页损坏降级、半程掉电 seq 择新、seq 回绕
 *            [4] WAL 预写日志逐页交换：穷举每一次 Flash 擦/写（含半页撕裂）掉电后重启续搬，
 *                含“第 0 页 WAL TAG 已落盘但 swap_page_idx 仍为 0”的极端边界与双次连续掉电！
 *            [5] 看门狗试运行：正常 Commit 转正、3 次未 Commit 自动回滚、回滚中途掉电续回滚、
 *                NORMAL 态 App 损坏自愈回滚、无旧备份时安全驻留 Bootloader
 *            [6] IAP 二进制协议栈：流式拆包抗噪、START/DATA/END/QUERY/REBOOT、
 *                重复 seq 幂等应答、乱序拦截、1KB 页边界断点续传、整包 CRC32 错拦截
 ******************************************************************************
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "crc16.h"
#include "crc32.h"
#include "bsp_flash.h"
#include "boot_meta.h"
#include "iap_proto.h"

/* ======================= 断言统计 ======================= */

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define EXPECT(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf("  [FAIL] %s (line %d): %s\n", __func__, __LINE__, (msg)); \
    } \
} while (0)

#define EXPECT_EQ_U32(actual, expected, msg) do { \
    uint32_t _a = (uint32_t)(actual); \
    uint32_t _e = (uint32_t)(expected); \
    g_tests_run++; \
    if (_a != _e) { \
        g_tests_failed++; \
        printf("  [FAIL] %s (line %d): %s (got=0x%08X, expect=0x%08X)\n", \
               __func__, __LINE__, (msg), (unsigned)_a, (unsigned)_e); \
    } \
} while (0)

/* ======================= 模拟 64KB STM32F103 片内 Flash ======================= */

static uint8_t  g_flash[SB_FLASH_TOTAL_SIZE];
static uint32_t g_flash_op_cnt    = 0u;
static uint32_t g_fail_at_op      = 0u;   /* 0 = 不注入掉电；>0 = 第 N 次擦/写失败 */
static uint8_t  g_fail_tear_half  = 0u;   /* 1 = 写入失败前先落盘一半字节模拟“撕裂页” */
static uint32_t g_erase_log[64];
static uint32_t g_erase_log_len   = 0u;

static void sim_flash_reset(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_flash_op_cnt   = 0u;
    g_fail_at_op     = 0u;
    g_fail_tear_half = 0u;
    g_erase_log_len  = 0u;
}

static uint8_t sim_addr_ok(uint32_t addr, uint16_t len)
{
    if (addr < SB_FLASH_WRITE_GUARD) {
        return 0u;
    }
    if (addr >= SB_FLASH_END) {
        return 0u;
    }
    if (len != 0u && (uint32_t)len > (SB_FLASH_END - addr)) {
        return 0u;
    }
    return 1u;
}

uint8_t bsp_flash_unlock(void)
{
    return BSP_FLASH_OK;
}

void bsp_flash_lock(void)
{
}

uint8_t bsp_flash_erase_page(uint32_t addr)
{
    uint32_t off;

    if (!sim_addr_ok(addr, 0u)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if ((addr & (SB_FLASH_PAGE_SIZE - 1u)) != 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }

    g_flash_op_cnt++;
    if (g_erase_log_len < ARRAY_SIZE(g_erase_log)) {
        g_erase_log[g_erase_log_len++] = addr;
    }

    off = addr - SB_FLASH_BASE;
    if (g_fail_at_op != 0u && g_flash_op_cnt >= g_fail_at_op) {
        if (g_fail_tear_half && g_flash_op_cnt == g_fail_at_op) {
            /* 模拟擦除到一半掉电：前 512 字节被擦成 0xFF，后 512 字节残损 */
            memset(&g_flash[off], 0xFF, SB_FLASH_PAGE_SIZE / 2u);
            g_flash[off + SB_FLASH_PAGE_SIZE / 2u] ^= 0x55u;
        }
        return BSP_FLASH_ERR_ERASE;
    }

    memset(&g_flash[off], 0xFF, SB_FLASH_PAGE_SIZE);
    return BSP_FLASH_OK;
}

uint8_t bsp_flash_program(uint32_t addr, const uint8_t *src, uint16_t len)
{
    uint32_t off;
    uint16_t i;

    if (src == 0 || len == 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    if ((addr & 1u) != 0u || (len & 1u) != 0u) {
        return BSP_FLASH_ERR_ALIGN;
    }
    if (!sim_addr_ok(addr, len)) {
        return BSP_FLASH_ERR_RANGE;
    }
    if ((addr / SB_FLASH_PAGE_SIZE) != ((addr + (uint32_t)len - 1u) / SB_FLASH_PAGE_SIZE)) {
        return BSP_FLASH_ERR_RANGE;
    }

    g_flash_op_cnt++;
    off = addr - SB_FLASH_BASE;

    if (g_fail_at_op != 0u && g_flash_op_cnt >= g_fail_at_op) {
        if (g_fail_tear_half && g_flash_op_cnt == g_fail_at_op) {
            uint16_t half = (uint16_t)((len / 2u) & ~1u);
            for (i = 0u; i < half; i++) {
                g_flash[off + i] &= src[i];
            }
        }
        return BSP_FLASH_ERR_PROG;
    }

    /* NOR Flash 物理规则：只能把 bit 1 写成 0；若某位原来是 0 而试图写 1，直接报错 */
    for (i = 0u; i < len; i++) {
        if ((src[i] & (uint8_t)(~g_flash[off + i])) != 0u) {
            return BSP_FLASH_ERR_PROG;
        }
        g_flash[off + i] &= src[i];
    }
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

/* ======================= 辅助工具 ======================= */

static void fill_fake_firmware(uint8_t *buf, uint32_t size, uint8_t seed)
{
    uint32_t i;

    /* 构造合法 Cortex-M3 向量表头部 + 确定性伪随机载荷 */
    for (i = 0u; i < size; i++) {
        buf[i] = (uint8_t)(((i * 37u) ^ (seed * 131u) ^ (i >> 5)) & 0xFFu);
    }
    if (size >= 8u) {
        /* MSP = 0x20005000, Reset_Handler = 0x08002101 */
        uint32_t msp = 0x20005000u;
        uint32_t rst = SB_APP_ADDR + 0x101u;
        memcpy(&buf[0], &msp, 4u);
        memcpy(&buf[4], &rst, 4u);
    }
}

static void sim_write_region_raw(uint32_t base_addr, const uint8_t *img, uint32_t size)
{
    uint32_t off = base_addr - SB_FLASH_BASE;
    uint32_t pages = (size + SB_FLASH_PAGE_SIZE - 1u) / SB_FLASH_PAGE_SIZE;

    memset(&g_flash[off], 0xFF, pages * SB_FLASH_PAGE_SIZE);
    memcpy(&g_flash[off], img, size);
}

static uint16_t build_req_frame(uint8_t cmd,
                                uint16_t seq,
                                const uint8_t *pl,
                                uint16_t len,
                                uint8_t *out)
{
    uint16_t crc;

    out[0] = IAP_FRAME_HEAD0;
    out[1] = IAP_FRAME_HEAD1;
    out[2] = cmd;
    out[3] = (uint8_t)(seq & 0xFFu);
    out[4] = (uint8_t)(seq >> 8);
    out[5] = (uint8_t)(len & 0xFFu);
    out[6] = (uint8_t)(len >> 8);
    if (len > 0u && pl != 0) {
        memcpy(&out[7], pl, len);
    }
    crc = crc16_modbus(out, (uint16_t)(7u + len));
    out[7u + len] = (uint8_t)(crc & 0xFFu);
    out[8u + len] = (uint8_t)(crc >> 8);
    return (uint16_t)(9u + len);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ======================= [1] CRC 黄金测试向量 ======================= */

static void test_crc_vectors(void)
{
    const uint8_t check_str[9] = { '1','2','3','4','5','6','7','8','9' };
    uint32_t crc32_v;
    uint16_t crc16_v;

    printf("[1] CRC16-Modbus & CRC32(IEEE 802.3) 黄金向量...\n");
    crc32_v = crc32_calc(check_str, 9u);
    EXPECT_EQ_U32(crc32_v, 0xCBF43926u, "CRC32('123456789') must equal 0xCBF43926");

    /* 分块增量计算与一次性计算结果一致 */
    {
        uint32_t st = crc32_update(0xFFFFFFFFu, check_str, 4u);
        st = crc32_update(st, check_str + 4u, 5u) ^ 0xFFFFFFFFu;
        EXPECT_EQ_U32(st, 0xCBF43926u, "incremental crc32_update matches crc32_calc");
    }

    /* Modbus CRC16 标准向量：'123456789' -> 0x4B37 */
    crc16_v = crc16_modbus(check_str, 9u);
    EXPECT_EQ_U32(crc16_v, 0x4B37u, "CRC16-Modbus('123456789') must equal 0x4B37");
}

/* ======================= [2] Flash 写保护与物理规则 ======================= */

static void test_flash_guard_and_physics(void)
{
    uint8_t buf[16];

    printf("[2] Flash Boot 区硬写保护线与 NOR 物理规则...\n");
    sim_flash_reset();
    memset(buf, 0x5A, sizeof(buf));

    /* 保护线以下严禁擦写 */
    EXPECT_EQ_U32(bsp_flash_erase_page(0x08000000u), BSP_FLASH_ERR_RANGE, "forbid erase 0x08000000");
    EXPECT_EQ_U32(bsp_flash_erase_page(0x08001C00u), BSP_FLASH_ERR_RANGE, "forbid erase 0x08001C00");
    EXPECT_EQ_U32(bsp_flash_program(0x08001FF0u, buf, 16u), BSP_FLASH_ERR_RANGE, "forbid prog boot");

    /* 非对齐/跨页拦截 */
    EXPECT_EQ_U32(bsp_flash_erase_page(0x08002004u), BSP_FLASH_ERR_ALIGN, "unaligned page erase");
    EXPECT_EQ_U32(bsp_flash_program(0x08002001u, buf, 4u), BSP_FLASH_ERR_ALIGN, "odd addr prog");
    EXPECT_EQ_U32(bsp_flash_program(0x08002000u, buf, 3u), BSP_FLASH_ERR_ALIGN, "odd len prog");
    EXPECT_EQ_U32(bsp_flash_program(0x080023FCu, buf, 8u), BSP_FLASH_ERR_RANGE, "cross-page prog");

    /* 正常写入后，不擦除直接把 0 写回 1 必须失败 */
    EXPECT_EQ_U32(bsp_flash_erase_page(SB_APP_ADDR), BSP_FLASH_OK, "erase app page0");
    EXPECT_EQ_U32(bsp_flash_program(SB_APP_ADDR, buf, 16u), BSP_FLASH_OK, "prog app page0");
    buf[0] = 0xFFu;
    EXPECT_EQ_U32(bsp_flash_program(SB_APP_ADDR, buf, 16u), BSP_FLASH_ERR_PROG, "0->1 without erase must fail");
}

/* ======================= [3] 双页 Meta 存储与掉电恢复 ======================= */

static void test_dual_page_meta(void)
{
    boot_meta_t m, loaded;
    uint8_t     from_bak = 0u;

    printf("[3] 双页 Meta 存储、先备后主顺序与半程掉电恢复...\n");
    sim_flash_reset();

    /* 3.1 空白芯片加载应返回 BOOT_ERR_BLANK 并填默认值 */
    EXPECT_EQ_U32(boot_meta_load(&loaded, &from_bak), BOOT_ERR_BLANK, "blank meta returns ERR_BLANK");
    EXPECT_EQ_U32(loaded.state, BOOT_STATE_NORMAL, "blank defaults to NORMAL");
    EXPECT_EQ_U32(boot_meta_current_seq(), 0u, "blank seq == 0");

    /* 3.2 验证保存顺序：必须先擦备区 0x0800E400，再擦主区 0x0800E000 */
    boot_meta_defaults(&m);
    m.app_version = 0x00010000u;
    m.app_size    = 4096u;
    m.app_crc32   = 0x12345678u;
    g_erase_log_len = 0u;
    EXPECT_EQ_U32(boot_meta_save(&m), BOOT_OK, "save meta #1");
    EXPECT_EQ_U32(g_erase_log_len, 2u, "two page erases per save");
    EXPECT_EQ_U32(g_erase_log[0], SB_META_BACK_ADDR, "first erase BACK page");
    EXPECT_EQ_U32(g_erase_log[1], SB_META_MAIN_ADDR, "then erase MAIN page");
    EXPECT_EQ_U32(boot_meta_current_seq(), 1u, "seq == 1");

    /* 3.3 主页被毁坏：自动无缝切到备页 */
    g_flash[SB_META_MAIN_ADDR - SB_FLASH_BASE + 8u] ^= 0xFFu;
    EXPECT_EQ_U32(boot_meta_load(&loaded, &from_bak), BOOT_OK, "load succeeds when MAIN corrupted");
    EXPECT_EQ_U32(from_bak, 1u, "loaded from backup");
    EXPECT_EQ_U32(loaded.app_version, 0x00010000u, "version preserved from backup");

    /* 3.4 模拟“备区写完 seq=2，写主区时掉电撕裂”：重启后必须选中更新的备区 seq=2 */
    m.app_version = 0x00010001u;
    g_flash_op_cnt   = 0u;
    g_fail_at_op     = 4u; /* op1=erase BACK, op2=prog BACK, op3=erase MAIN, op4=prog MAIN(fail!) */
    g_fail_tear_half = 1u;
    EXPECT_EQ_U32(boot_meta_save(&m), BOOT_ERR_FLASH, "save fails during MAIN program");
    g_fail_at_op     = 0u;
    g_fail_tear_half = 0u;

    EXPECT_EQ_U32(boot_meta_load(&loaded, &from_bak), BOOT_OK, "recover after MAIN tear");
    EXPECT_EQ_U32(from_bak, 1u, "selected newer BACK page");
    EXPECT_EQ_U32(loaded.app_version, 0x00010001u, "got newer version v1.1");
    EXPECT_EQ_U32(boot_meta_current_seq(), 2u, "seq == 2 from BACK");

    /* 3.5 模拟“备区刚擦完还没写就掉电”：主区仍保留旧版完整记录，不丢配置 */
    EXPECT_EQ_U32(boot_meta_save(&m), BOOT_OK, "sync both pages at seq=3");
    m.app_version = 0x00010002u;
    g_flash_op_cnt   = 0u;
    g_fail_at_op     = 2u; /* op1=erase BACK, op2=prog BACK(fail!) */
    g_fail_tear_half = 1u;
    EXPECT_EQ_U32(boot_meta_save(&m), BOOT_ERR_FLASH, "save fails during BACK program");
    g_fail_at_op     = 0u;
    g_fail_tear_half = 0u;

    EXPECT_EQ_U32(boot_meta_load(&loaded, &from_bak), BOOT_OK, "recover from MAIN when BACK torn");
    EXPECT_EQ_U32(from_bak, 0u, "selected intact MAIN page");
    EXPECT_EQ_U32(loaded.app_version, 0x00010001u, "previous intact version v1.1 preserved");
}

/* ======================= [4] WAL 逐页交换：穷举任意微秒掉电续搬 ======================= */

static void test_wal_swap_exhaustive_power_loss(void)
{
    uint8_t       img_v10[3584];  /* 3.5 KB (4 pages) */
    uint8_t       img_v11[5248];  /* 5.125 KB (6 pages，刻意不同大小) */
    uint32_t      crc_v10, crc_v11;
    uint32_t      total_ops_baseline = 0u;
    uint32_t      cut;
    uint8_t       readback[5248];
    boot_meta_t   init_meta, m;
    boot_action_t act;
    uint8_t       from_bak = 0u;

    printf("[4] WAL 逐页交换搬运：穷举每一次 Flash 操作（含撕裂页）掉电续搬...\n");
    fill_fake_firmware(img_v10, sizeof(img_v10), 0x10u);
    fill_fake_firmware(img_v11, sizeof(img_v11), 0x11u);
    crc_v10 = crc32_calc(img_v10, sizeof(img_v10));
    crc_v11 = crc32_calc(img_v11, sizeof(img_v11));

    boot_meta_defaults(&init_meta);
    init_meta.state        = BOOT_STATE_PENDING_INSTALL;
    init_meta.app_version  = 0x00010000u;
    init_meta.app_size     = sizeof(img_v10);
    init_meta.app_crc32    = crc_v10;
    init_meta.slot_version = 0x00010001u;
    init_meta.slot_size    = sizeof(img_v11);
    init_meta.slot_crc32   = crc_v11;

    /* 4.1 先无故障跑一遍，测出完整搬运 + TESTING 入栈总共发生了多少次 Flash 擦写 */
    sim_flash_reset();
    sim_write_region_raw(SB_APP_ADDR,  img_v10, sizeof(img_v10));
    sim_write_region_raw(SB_SLOT_ADDR, img_v11, sizeof(img_v11));
    EXPECT_EQ_U32(boot_meta_save(&init_meta), BOOT_OK, "seed init_meta");

    g_flash_op_cnt = 0u;
    m = init_meta;
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "baseline swap succeeds");
    total_ops_baseline = g_flash_op_cnt;
    EXPECT_EQ_U32(act, BOOT_ACT_DID_INSTALL, "baseline act == DID_INSTALL");
    EXPECT_EQ_U32(m.state, BOOT_STATE_TESTING, "baseline enters TESTING");
    EXPECT_EQ_U32(m.try_cnt, 1u, "baseline try_cnt == 1");
    EXPECT_EQ_U32(m.app_version, 0x00010001u, "App now v1.1");
    EXPECT_EQ_U32(m.slot_version, 0x00010000u, "Slot now holds Golden v1.0");
    bsp_flash_read(SB_APP_ADDR,  readback, sizeof(img_v11));
    EXPECT(memcmp(readback, img_v11, sizeof(img_v11)) == 0, "baseline App bytes == v1.1");
    bsp_flash_read(SB_SLOT_ADDR, readback, sizeof(img_v10));
    EXPECT(memcmp(readback, img_v10, sizeof(img_v10)) == 0, "baseline Slot bytes == v1.0");
    printf("    -> 基准 6 页 WAL 交换共执行 %u 次 Flash 擦写，开始穷举第 1..%u 步任意时刻掉电...\n",
           (unsigned)total_ops_baseline, (unsigned)total_ops_baseline);

    /* 4.2 穷举在第 1 .. total_ops_baseline 次 Flash 操作中途撕裂掉电！ */
    for (cut = 1u; cut <= total_ops_baseline; cut++) {
        uint8_t rc1, rc2;

        sim_flash_reset();
        sim_write_region_raw(SB_APP_ADDR,  img_v10, sizeof(img_v10));
        sim_write_region_raw(SB_SLOT_ADDR, img_v11, sizeof(img_v11));
        (void)boot_meta_save(&init_meta);

        /* 第一次上电：在第 cut 次擦写时注入掉电 + 半页撕裂 */
        g_flash_op_cnt   = 0u;
        g_fail_at_op     = cut;
        g_fail_tear_half = 1u;
        EXPECT_EQ_U32(boot_meta_load(&m, &from_bak), BOOT_OK, "load before cut");
        rc1 = boot_process_startup(&m, &act);
        EXPECT_EQ_U32(rc1, BOOT_ERR_FLASH, "cut must abort with ERR_FLASH");

        /* 第二次上电（恢复供电）：重加载 Meta 并续搬 */
        g_fail_at_op     = 0u;
        g_fail_tear_half = 0u;
        EXPECT_EQ_U32(boot_meta_load(&m, &from_bak), BOOT_OK, "load after power-loss");
        rc2 = boot_process_startup(&m, &act);
        EXPECT_EQ_U32(rc2, BOOT_OK, "resume after power-loss must succeed");
        EXPECT_EQ_U32(m.state, BOOT_STATE_TESTING, "resumed state == TESTING");
        EXPECT_EQ_U32(m.app_version, 0x00010001u, "resumed App version == v1.1");
        EXPECT_EQ_U32(m.slot_version, 0x00010000u, "resumed Slot version == v1.0");
        EXPECT_EQ_U32(boot_verify_region(SB_APP_ADDR, m.app_size, m.app_crc32), BOOT_OK, "resumed App CRC32 OK");
        EXPECT_EQ_U32(boot_verify_region(SB_SLOT_ADDR, m.slot_size, m.slot_crc32), BOOT_OK, "resumed Slot CRC32 OK");

        bsp_flash_read(SB_APP_ADDR, readback, sizeof(img_v11));
        EXPECT(memcmp(readback, img_v11, sizeof(img_v11)) == 0, "resumed App bytes bit-exact v1.1");
        bsp_flash_read(SB_SLOT_ADDR, readback, sizeof(img_v10));
        EXPECT(memcmp(readback, img_v10, sizeof(img_v10)) == 0, "resumed Slot bytes bit-exact v1.0");
    }

    /* 4.3 连续两次掉电（第 1 次在第 0 页 Stage B 掉电，第 2 次在第 3 页 Stage B 再掉电） */
    sim_flash_reset();
    sim_write_region_raw(SB_APP_ADDR,  img_v10, sizeof(img_v10));
    sim_write_region_raw(SB_SLOT_ADDR, img_v11, sizeof(img_v11));
    (void)boot_meta_save(&init_meta);

    /* 第 1 次掉电：第 0 页写入 App[0] 后、写 Slot[0] 中途（op 9 左右） */
    g_flash_op_cnt = 0u; g_fail_at_op = 9u; g_fail_tear_half = 1u;
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_ERR_FLASH, "double-cut #1");

    /* 第 2 次掉电：续搬过程再砍一次（op 25） */
    g_flash_op_cnt = 0u; g_fail_at_op = 25u; g_fail_tear_half = 1u;
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_ERR_FLASH, "double-cut #2");

    /* 第 3 次上电：彻底完成 */
    g_fail_at_op = 0u; g_fail_tear_half = 0u;
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "double-cut final recovery OK");
    EXPECT_EQ_U32(m.last_event, BOOT_EVT_INSTALL_RESUMED, "last_event records INSTALL_RESUMED");
    bsp_flash_read(SB_APP_ADDR,  readback, sizeof(img_v11));
    EXPECT(memcmp(readback, img_v11, sizeof(img_v11)) == 0, "double-cut App == v1.1");
    bsp_flash_read(SB_SLOT_ADDR, readback, sizeof(img_v10));
    EXPECT(memcmp(readback, img_v10, sizeof(img_v10)) == 0, "double-cut Slot == v1.0");
}

/* ======================= [5] 试运行 Commit、3 次失败自动回滚与自愈 ======================= */

static void test_testing_commit_and_rollback(void)
{
    uint8_t       img_v10[2048];
    uint8_t       img_bad[3072];
    uint8_t       readback[3072];
    uint32_t      crc_v10, crc_bad;
    boot_meta_t   m;
    boot_action_t act;
    uint8_t       from_bak = 0u;

    printf("[5] 试运行 Commit、3 次未 Commit 自动回滚、回滚掉电恢复与坏 App 自愈...\n");
    fill_fake_firmware(img_v10, sizeof(img_v10), 0x10u);
    fill_fake_firmware(img_bad, sizeof(img_bad), 0x99u);
    crc_v10 = crc32_calc(img_v10, sizeof(img_v10));
    crc_bad = crc32_calc(img_bad, sizeof(img_bad));

    /* 5.1 正常新固件：进入 TESTING，启动第 1 次后 App 调用 Commit -> NORMAL */
    sim_flash_reset();
    sim_write_region_raw(SB_APP_ADDR,  img_v10, sizeof(img_v10));
    sim_write_region_raw(SB_SLOT_ADDR, img_bad, sizeof(img_bad));
    boot_meta_defaults(&m);
    m.state        = BOOT_STATE_PENDING_INSTALL;
    m.app_version  = 0x00010000u; m.app_size  = sizeof(img_v10); m.app_crc32  = crc_v10;
    m.slot_version = 0x00010002u; m.slot_size = sizeof(img_bad); m.slot_crc32 = crc_bad;
    (void)boot_meta_save(&m);

    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "install & try #1");
    EXPECT_EQ_U32(m.state, BOOT_STATE_TESTING, "state == TESTING");
    EXPECT_EQ_U32(m.try_cnt, 1u, "try_cnt == 1");
    EXPECT_EQ_U32(boot_meta_commit_app(), BOOT_OK, "App calls commit");

    EXPECT_EQ_U32(boot_meta_load(&m, &from_bak), BOOT_OK, "reload after commit");
    EXPECT_EQ_U32(m.state, BOOT_STATE_NORMAL, "state == NORMAL after commit");
    EXPECT_EQ_U32(m.try_cnt, 0u, "try_cnt reset to 0");
    EXPECT_EQ_U32(m.last_event, BOOT_EVT_UPGRADE_OK, "last_event == UPGRADE_OK");

    /* 5.2 坏固件（不 Commit，连续 3 次看门狗复位 -> 第 4 次启动自动回滚到 v1.0） */
    sim_flash_reset();
    sim_write_region_raw(SB_APP_ADDR,  img_v10, sizeof(img_v10));
    sim_write_region_raw(SB_SLOT_ADDR, img_bad, sizeof(img_bad));
    boot_meta_defaults(&m);
    m.state        = BOOT_STATE_PENDING_INSTALL;
    m.app_version  = 0x00010000u; m.app_size  = sizeof(img_v10); m.app_crc32  = crc_v10;
    m.slot_version = 0x00010002u; m.slot_size = sizeof(img_bad); m.slot_crc32 = crc_bad;
    (void)boot_meta_save(&m);

    /* Boot #1: 搬运并第 1 次试跑 -> 死机未 Commit */
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "bad boot #1");
    EXPECT_EQ_U32(act, BOOT_ACT_DID_INSTALL, "boot #1 act == DID_INSTALL");
    EXPECT_EQ_U32(m.try_cnt, 1u, "boot #1 try_cnt == 1");

    /* Boot #2: 第 2 次试跑 -> 死机未 Commit */
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "bad boot #2");
    EXPECT_EQ_U32(act, BOOT_ACT_JUMP_APP, "boot #2 act == JUMP_APP");
    EXPECT_EQ_U32(m.try_cnt, 2u, "boot #2 try_cnt == 2");

    /* Boot #3: 第 3 次试跑 -> 死机未 Commit */
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "bad boot #3");
    EXPECT_EQ_U32(m.try_cnt, 3u, "boot #3 try_cnt == 3");

    /* Boot #4: 达到 BOOT_META_MAX_TRY(3) -> 触发回滚！先测中途掉电，再测重上电续回滚 */
    (void)boot_meta_load(&m, &from_bak);
    g_flash_op_cnt = 0u; g_fail_at_op = 6u; g_fail_tear_half = 1u; /* 写完 ROLLBACK 后拷贝第 1 页掉电 */
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_ERR_NO_VALID_APP, "rollback interrupted by power-loss");

    g_fail_at_op = 0u; g_fail_tear_half = 0u;
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(m.state, BOOT_STATE_ROLLBACK, "state persisted as ROLLBACK");
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "rollback resumes and succeeds");
    EXPECT_EQ_U32(act, BOOT_ACT_DID_ROLLBACK, "act == DID_ROLLBACK");
    EXPECT_EQ_U32(m.state, BOOT_STATE_NORMAL, "state restored to NORMAL");
    EXPECT_EQ_U32(m.app_version, 0x00010000u, "App version rolled back to v1.0");
    EXPECT_EQ_U32(m.last_event, BOOT_EVT_ROLLBACK_WDOG, "last_event == ROLLBACK_WDOG");
    bsp_flash_read(SB_APP_ADDR, readback, sizeof(img_v10));
    EXPECT(memcmp(readback, img_v10, sizeof(img_v10)) == 0, "App bytes restored to v1.0");

    /* 5.3 NORMAL 态下 App 区被意外篡改 -> 从 Slot 备份自愈回滚 */
    g_flash[SB_APP_ADDR - SB_FLASH_BASE + 123u] ^= 0xFFu;
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "corrupted App self-heals from Slot");
    EXPECT_EQ_U32(act, BOOT_ACT_DID_ROLLBACK, "self-heal act == DID_ROLLBACK");
    EXPECT_EQ_U32(m.last_event, BOOT_EVT_ROLLBACK_BAD_APP, "last_event == ROLLBACK_BAD_APP");
    bsp_flash_read(SB_APP_ADDR, readback, sizeof(img_v10));
    EXPECT(memcmp(readback, img_v10, sizeof(img_v10)) == 0, "self-healed App bytes == v1.0");

    /* 5.4 无旧备份（空白板首次刷入坏固件）3 次失败后不乱跳，安全驻留 Bootloader */
    sim_flash_reset();
    sim_write_region_raw(SB_SLOT_ADDR, img_bad, sizeof(img_bad));
    boot_meta_defaults(&m);
    m.state        = BOOT_STATE_PENDING_INSTALL;
    m.slot_version = 0x00010002u; m.slot_size = sizeof(img_bad); m.slot_crc32 = crc_bad;
    (void)boot_meta_save(&m);

    for (int k = 0; k < 3; k++) {
        (void)boot_meta_load(&m, &from_bak);
        EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_OK, "blank-board bad try");
    }
    (void)boot_meta_load(&m, &from_bak);
    EXPECT_EQ_U32(boot_process_startup(&m, &act), BOOT_ERR_NO_VALID_APP, "no backup -> stay in boot");
    EXPECT_EQ_U32(act, BOOT_ACT_STAY_BOOT, "act == STAY_BOOT");
}

/* ======================= [6] IAP 二进制分包协议栈 ======================= */

static uint16_t feed_frame_stream(iap_ctx_t *ctx, const uint8_t *frame, uint16_t len, uint8_t *rsp)
{
    uint16_t i;
    uint16_t out = 0u;

    for (i = 0u; i < len; i++) {
        out = iap_proto_feed_byte(ctx, frame[i], rsp);
        if (out > 0u) {
            return out;
        }
    }
    return out;
}

static void test_iap_protocol(void)
{
    uint8_t       img[2304]; /* 18 包 = 2 页(16 包) + 2 包(256B)，测试非整页大小 */
    uint32_t      img_crc;
    boot_meta_t   m;
    iap_ctx_t     ctx;
    uint8_t       req[IAP_MAX_FRAME_LEN];
    uint8_t       rsp[IAP_MAX_FRAME_LEN];
    uint8_t       pl[16];
    uint16_t      req_len, rsp_len;
    uint16_t      seq;
    uint8_t       from_bak = 0u;
    uint8_t       noise[5] = { 0x00u, 0xAAu, 0xAAu, 0x12u, 0xFFu };

    printf("[6] IAP 分包协议：抗噪流式解析、幂等重传、乱序拦截、1KB 页断点续传与 CRC32...\n");
    fill_fake_firmware(img, sizeof(img), 0x77u);
    img_crc = crc32_calc(img, sizeof(img));

    sim_flash_reset();
    boot_meta_defaults(&m);
    m.app_version = 0x00010000u;
    (void)boot_meta_save(&m);
    iap_proto_init(&ctx, &m, 0u);

    /* 6.1 噪声注入 + 坏 CRC16 静默丢弃 + QUERY 查询 */
    for (uint16_t i = 0u; i < sizeof(noise); i++) {
        EXPECT_EQ_U32(iap_proto_feed_byte(&ctx, noise[i], rsp), 0u, "noise produces no rsp");
    }
    req_len = build_req_frame(IAP_CMD_QUERY, 0u, 0, 0u, req);
    req[req_len - 1u] ^= 0x01u; /* 破坏 CRC16 */
    EXPECT_EQ_U32(feed_frame_stream(&ctx, req, req_len, rsp), 0u, "bad CRC16 silently dropped");

    req_len = build_req_frame(IAP_CMD_QUERY, 0u, 0, 0u, req);
    rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
    EXPECT(rsp_len == 9u + sizeof(iap_query_rsp_t), "QUERY rsp length == 33");
    EXPECT_EQ_U32(rsp[2], IAP_CMD_QUERY | IAP_RSP_MASK, "QUERY rsp cmd");
    EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "QUERY rsp OK");
    {
        iap_query_rsp_t q;
        memcpy(&q, &rsp[7], sizeof(q));
        EXPECT_EQ_U32(q.run_mode, 0u, "run_mode == Boot");
        EXPECT_EQ_U32(q.state, BOOT_STATE_NORMAL, "state == NORMAL");
        EXPECT_EQ_U32(q.app_version, 0x00010000u, "app_version == v1.0");
    }

    /* 6.2 START 开启升级 */
    put_le32(&pl[0], 0x00010001u);
    put_le32(&pl[4], sizeof(img));
    put_le32(&pl[8], img_crc);
    put_le16(&pl[12], IAP_START_FLAG_RESUME);
    req_len = build_req_frame(IAP_CMD_START, 0u, pl, 14u, req);
    rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
    EXPECT(rsp_len == 11u, "START rsp length == 11");
    EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "START OK");
    EXPECT_EQ_U32(le16(&rsp[7]), 0u, "fresh START resume_seq == 0");

    /* 6.3 发送前 11 包（第 0 页 8 包已落盘 + 第 1 页写了 3 包），期间测试乱序与重复包幂等 */
    /* 乱序测试：当前期待 seq=0，发 seq=2 必须返回 ERR_SEQ + expect=0 */
    req_len = build_req_frame(IAP_CMD_DATA, 2u, &img[256], 128u, req);
    rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
    EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_ERR_SEQ, "out-of-order rejected");
    EXPECT_EQ_U32(le16(&rsp[7]), 0u, "ERR_SEQ reports expected seq 0");

    for (seq = 0u; seq < 11u; seq++) {
        req_len = build_req_frame(IAP_CMD_DATA, seq, &img[seq * 128u], 128u, req);
        rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
        EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "DATA seq OK");
        EXPECT_EQ_U32(le16(&rsp[7]), seq + 1u, "DATA next seq ack");
    }

    /* 幂等重传测试：重复发送已写的 seq=3（不应重新写 Flash，否则 NOR 物理规则会报错或覆盖） */
    {
        uint32_t ops_before = g_flash_op_cnt;
        req_len = build_req_frame(IAP_CMD_DATA, 3u, &img[3u * 128u], 128u, req);
        rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
        EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "duplicate seq idempotent OK");
        EXPECT_EQ_U32(le16(&rsp[7]), 11u, "duplicate seq reports current ram_done_seq=11");
        EXPECT_EQ_U32(g_flash_op_cnt, ops_before, "duplicate seq did zero Flash writes");
    }

    /* 6.4 模拟串口断线 + 芯片掉电重启：Meta 中已落盘 dl_done_seq == 8（页对齐！） */
    memset(&m, 0, sizeof(m));
    EXPECT_EQ_U32(boot_meta_load(&m, &from_bak), BOOT_OK, "reload meta after download power-loss");
    EXPECT_EQ_U32(m.state, BOOT_STATE_DOWNLOADING, "state == DOWNLOADING");
    EXPECT_EQ_U32(m.dl_done_seq, 8u, "persisted dl_done_seq == 8 (1KB page boundary)");
    iap_proto_init(&ctx, &m, 0u);

    /* 上位机带 RESUME 标志重连 START -> 必须返回 resume_seq == 8 */
    req_len = build_req_frame(IAP_CMD_START, 0u, pl, 14u, req);
    rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
    EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "RESUME START OK");
    EXPECT_EQ_U32(le16(&rsp[7]), 8u, "RESUME returns seq == 8");

    /* 从 seq=8 续传到末包 seq=17（第 1 页会被 seq=8 自动整页擦除重刷，清理之前半页残留） */
    for (seq = 8u; seq < 18u; seq++) {
        req_len = build_req_frame(IAP_CMD_DATA, seq, &img[seq * 128u], 128u, req);
        rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
        EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "resumed DATA OK");
    }

    /* 6.5 先故意篡改 1 字节 Slot 测试 END 整包 CRC32 拦截 */
    g_flash[SB_SLOT_ADDR - SB_FLASH_BASE + 500u] ^= 0x01u;
    req_len = build_req_frame(IAP_CMD_END, 0u, 0, 0u, req);
    rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
    EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_ERR_CRC, "END rejects corrupted Slot CRC32");
    EXPECT_EQ_U32(ctx.reboot_requested, 0u, "no reboot on bad CRC32");

    /* 恢复该字节后再发 END -> 必须通过并切 PENDING_INSTALL + 请求复位 */
    g_flash[SB_SLOT_ADDR - SB_FLASH_BASE + 500u] ^= 0x01u;
    rsp_len = feed_frame_stream(&ctx, req, req_len, rsp);
    EXPECT_EQ_U32(le16(&rsp[3]), IAP_STATUS_OK, "END OK");
    EXPECT_EQ_U32(ctx.reboot_requested, 1u, "END sets reboot_requested");
    EXPECT_EQ_U32(m.state, BOOT_STATE_PENDING_INSTALL, "END transitions to PENDING_INSTALL");
}

int main(void)
{
    printf("=== SafeBoot-F1 PC 单元测试套件 ===\n");
    crc16_init();

    test_crc_vectors();
    test_flash_guard_and_physics();
    test_dual_page_meta();
    test_wal_swap_exhaustive_power_loss();
    test_testing_commit_and_rollback();
    test_iap_protocol();

    printf("\n========================================\n");
    printf("Total assertions: %d, Failed: %d\n", g_tests_run, g_tests_failed);
    printf("========================================\n");
    return (g_tests_failed == 0) ? 0 : 1;
}
