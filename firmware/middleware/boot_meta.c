/**
 ******************************************************************************
 * @file    boot_meta.c
 * @brief   Boot Meta 双页存储与防变砖五态状态机实现
 * @note    SafeBoot-F1
 ******************************************************************************
 */
#include "boot_meta.h"
#include "bsp_flash.h"
#include "crc16.h"
#include "crc32.h"
#include <string.h>

STATIC_ASSERT(sizeof(boot_meta_t) == 36,     boot_meta_size_must_be_36);
STATIC_ASSERT(sizeof(boot_meta_rec_t) == 56, boot_meta_rec_size_must_be_56);

#define REC_CRC_LEN     52u     /* sizeof(boot_meta_rec_t) - 4 */

/* ======================= 内部工具 ======================= */

static uint8_t seq_is_newer(uint32_t a, uint32_t b)
{
    return (((int32_t)(a - b)) > 0) ? 1u : 0u;
}

static void rec_build(boot_meta_rec_t *r, const boot_meta_t *m, uint32_t seq)
{
    memset(r, 0, sizeof(*r));
    r->magic        = BOOT_META_MAGIC;
    r->seq          = seq;
    r->layout_ver   = BOOT_META_LAYOUT_VER;
    r->payload_size = (uint16_t)sizeof(boot_meta_t);
    r->reserved_hdr = 0u;
    r->meta         = *m;
    r->crc16        = crc16_modbus((const uint8_t *)r, REC_CRC_LEN);
    r->pad          = 0xFFFFu;
}

static uint8_t rec_check(const boot_meta_rec_t *r)
{
    uint16_t calc;

    if (r->magic == 0xFFFFFFFFu) {
        return BOOT_ERR_BLANK;
    }
    if (r->magic != BOOT_META_MAGIC) {
        return BOOT_ERR_MAGIC;
    }
    if (r->layout_ver != BOOT_META_LAYOUT_VER ||
        r->payload_size != (uint16_t)sizeof(boot_meta_t)) {
        return BOOT_ERR_PARAM;
    }
    if (r->meta.state > (uint16_t)BOOT_STATE_ROLLBACK) {
        return BOOT_ERR_PARAM;
    }
    calc = crc16_modbus((const uint8_t *)r, REC_CRC_LEN);
    if (calc != r->crc16) {
        return BOOT_ERR_CRC;
    }
    return BOOT_OK;
}

static uint8_t write_rec(uint32_t page_addr, const boot_meta_rec_t *r)
{
    boot_meta_rec_t rb;

    if (bsp_flash_erase_page(page_addr) != BSP_FLASH_OK) {
        return BOOT_ERR_FLASH;
    }
    if (bsp_flash_program(page_addr, (const uint8_t *)r, (uint16_t)sizeof(*r)) != BSP_FLASH_OK) {
        return BOOT_ERR_FLASH;
    }
    bsp_flash_read(page_addr, (uint8_t *)&rb, (uint16_t)sizeof(rb));
    if (rec_check(&rb) != BOOT_OK || rb.seq != r->seq) {
        return BOOT_ERR_FLASH;
    }
    return BOOT_OK;
}

uint32_t boot_meta_current_seq(void)
{
    boot_meta_rec_t rm, rb;
    uint8_t         ok_m, ok_b;

    bsp_flash_read(SB_META_MAIN_ADDR, (uint8_t *)&rm, (uint16_t)sizeof(rm));
    bsp_flash_read(SB_META_BACK_ADDR, (uint8_t *)&rb, (uint16_t)sizeof(rb));
    ok_m = (rec_check(&rm) == BOOT_OK) ? 1u : 0u;
    ok_b = (rec_check(&rb) == BOOT_OK) ? 1u : 0u;

    if (ok_m && ok_b) {
        return seq_is_newer(rb.seq, rm.seq) ? rb.seq : rm.seq;
    }
    if (ok_m) {
        return rm.seq;
    }
    if (ok_b) {
        return rb.seq;
    }
    return 0u;
}

void boot_meta_defaults(boot_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->state = (uint16_t)BOOT_STATE_NORMAL;
}

uint8_t boot_meta_load(boot_meta_t *out, uint8_t *out_from_backup)
{
    boot_meta_rec_t rm, rb;
    uint8_t         ok_m, ok_b;

    if (out_from_backup != 0) {
        *out_from_backup = 0u;
    }
    bsp_flash_read(SB_META_MAIN_ADDR, (uint8_t *)&rm, (uint16_t)sizeof(rm));
    bsp_flash_read(SB_META_BACK_ADDR, (uint8_t *)&rb, (uint16_t)sizeof(rb));
    ok_m = (rec_check(&rm) == BOOT_OK) ? 1u : 0u;
    ok_b = (rec_check(&rb) == BOOT_OK) ? 1u : 0u;

    if (ok_m && ok_b) {
        if (seq_is_newer(rb.seq, rm.seq)) {
            *out = rb.meta;
            if (out_from_backup != 0) { *out_from_backup = 1u; }
        } else {
            *out = rm.meta;
        }
        return BOOT_OK;
    }
    if (ok_m) {
        *out = rm.meta;
        return BOOT_OK;
    }
    if (ok_b) {
        *out = rb.meta;
        if (out_from_backup != 0) { *out_from_backup = 1u; }
        return BOOT_OK;
    }

    boot_meta_defaults(out);
    return BOOT_ERR_BLANK;
}

uint8_t boot_meta_save(const boot_meta_t *m)
{
    boot_meta_rec_t rec;
    uint32_t        seq = boot_meta_current_seq() + 1u;

    rec_build(&rec, m, seq);

    /* 先写备区，备区成功再写主区：任何时刻掉电至少有一份完整记录 */
    if (write_rec(SB_META_BACK_ADDR, &rec) != BOOT_OK) {
        return BOOT_ERR_FLASH;
    }
    if (write_rec(SB_META_MAIN_ADDR, &rec) != BOOT_OK) {
        return BOOT_ERR_FLASH;
    }
    return BOOT_OK;
}

/* ======================= 固件区校验与搬运 ======================= */

uint8_t boot_verify_region(uint32_t base_addr, uint32_t size, uint32_t expect_crc32)
{
    uint32_t calc;

    if (size == 0u || size > SB_APP_SIZE || (size & 1u) != 0u) {
        return BOOT_ERR_PARAM;
    }
    calc = bsp_flash_crc32(base_addr, size);
    return (calc == expect_crc32) ? BOOT_OK : BOOT_ERR_CRC;
}

/**
 * @brief  逐页交换 App 区与 Slot 区（带暂存页掉电保护 + 进度落盘）
 * @note   为什么借用 SB_RSVD_ADDR（0x0800E800）做暂存页：
 *         如果只用 RAM 互换 App[i] 与 Slot[i]，在擦空 App[i] 还没写完时断电，
 *         App[i] 那 1KB 就会丢失。
 *         采用“先把旧 App[i] 备份到 RSVD 暂存页 → 再把 Slot[i] 写入 App[i] →
 *         再把 RSVD 写入 Slot[i] → 更新 swap_page_idx”的三步法，或直接把 Slot 拷入 App，
 *         哪怕在任意一个微秒拔掉电源，重上电都能接着搬完！
 *
 *         更直接且磨损更小的做法：
 *         - 安装阶段（PENDING_INSTALL）：首次进入时，如果旧 App 有效且我们需要保留回滚能力，
 *           逐页借助 RSVD_ADDR 交换 App[i] 与 Slot[i]，每换完 1 页更新 swap_page_idx。
 *         - 回滚阶段（ROLLBACK）：直接把 Slot（旧好固件）逐页复制回 App 区。
 */
static uint8_t copy_page(uint32_t dst_page, uint32_t src_page)
{
    uint8_t buf[SB_FLASH_PAGE_SIZE];

    bsp_flash_read(src_page, buf, SB_FLASH_PAGE_SIZE);
    if (bsp_flash_erase_page(dst_page) != BSP_FLASH_OK) {
        return BOOT_ERR_FLASH;
    }
    if (bsp_flash_program(dst_page, buf, SB_FLASH_PAGE_SIZE) != BSP_FLASH_OK) {
        return BOOT_ERR_FLASH;
    }
    return BOOT_OK;
}

#define WAL_SCRATCH_OLD_ADDR    0x0800E800u
#define WAL_SCRATCH_NEW_ADDR    0x0800EC00u
#define WAL_SCRATCH_TAG_ADDR    0x0800F000u
#define WAL_TAG_MAGIC           0x53570000u     /* "SW" << 16 | page_idx */

static uint8_t swap_app_and_slot(boot_meta_t *m)
{
    uint32_t max_bytes = MAX_OF(m->app_size, m->slot_size);
    uint16_t total_pages;
    uint16_t i;
    uint32_t app_p, slot_p;
    uint32_t tag_val, expect_tag;
    uint32_t tmp_ver, tmp_sz, tmp_crc;
    uint8_t  resumed = (m->swap_page_idx > 0u) ? 1u : 0u;

    if (max_bytes == 0u || max_bytes > SB_APP_SIZE) {
        max_bytes = SB_APP_SIZE;
    }
    total_pages = (uint16_t)((max_bytes + SB_FLASH_PAGE_SIZE - 1u) / SB_FLASH_PAGE_SIZE);

    for (i = m->swap_page_idx; i < total_pages; i++) {
        app_p      = SB_APP_ADDR  + (uint32_t)i * SB_FLASH_PAGE_SIZE;
        slot_p     = SB_SLOT_ADDR + (uint32_t)i * SB_FLASH_PAGE_SIZE;
        expect_tag = WAL_TAG_MAGIC | (uint32_t)i;

        /*
         * WAL 预写日志式双暂存页交换（任意微秒断电 100% 可恢复）：
         *   阶段 A（建快照）：若 TAG != expect_tag，说明 App[i]/Slot[i] 尚未改动，
         *                     先将 App[i]->SCRATCH_OLD、Slot[i]->SCRATCH_NEW，最后写 TAG。
         *   阶段 B（落目标）：TAG 已就位，将 SCRATCH_NEW->App[i]、SCRATCH_OLD->Slot[i]，
         *                     若在此期间断电，重启后见 TAG==expect_tag，跳过阶段 A 直接重刷阶段 B。
         */
        bsp_flash_read(WAL_SCRATCH_TAG_ADDR, (uint8_t *)&tag_val, (uint16_t)sizeof(tag_val));
        if (tag_val != expect_tag) {
            if (copy_page(WAL_SCRATCH_OLD_ADDR, app_p) != BOOT_OK) {
                return BOOT_ERR_FLASH;
            }
            if (copy_page(WAL_SCRATCH_NEW_ADDR, slot_p) != BOOT_OK) {
                return BOOT_ERR_FLASH;
            }
            if (bsp_flash_erase_page(WAL_SCRATCH_TAG_ADDR) != BSP_FLASH_OK) {
                return BOOT_ERR_FLASH;
            }
            if (bsp_flash_program(WAL_SCRATCH_TAG_ADDR,
                                  (const uint8_t *)&expect_tag,
                                  (uint16_t)sizeof(expect_tag)) != BSP_FLASH_OK) {
                return BOOT_ERR_FLASH;
            }
        } else {
            resumed = 1u;
        }

        if (copy_page(app_p, WAL_SCRATCH_NEW_ADDR) != BOOT_OK) {
            return BOOT_ERR_FLASH;
        }
        if (copy_page(slot_p, WAL_SCRATCH_OLD_ADDR) != BOOT_OK) {
            return BOOT_ERR_FLASH;
        }

        m->swap_page_idx = (uint16_t)(i + 1u);
        if (boot_meta_save(m) != BOOT_OK) {
            return BOOT_ERR_FLASH;
        }
    }

    /* 清空 WAL TAG 页，互换 app_* 与 slot_* 元数据，进入 STATE_TESTING */
    if (bsp_flash_erase_page(WAL_SCRATCH_TAG_ADDR) != BSP_FLASH_OK) {
        return BOOT_ERR_FLASH;
    }

    tmp_ver = m->app_version;  m->app_version  = m->slot_version;  m->slot_version  = tmp_ver;
    tmp_sz  = m->app_size;     m->app_size     = m->slot_size;     m->slot_size     = tmp_sz;
    tmp_crc = m->app_crc32;    m->app_crc32    = m->slot_crc32;    m->slot_crc32    = tmp_crc;

    m->state         = (uint16_t)BOOT_STATE_TESTING;
    m->try_cnt       = 0u;
    m->swap_page_idx = 0u;
    m->dl_done_seq   = 0u;
    if (resumed != 0u) {
        m->last_event = BOOT_EVT_INSTALL_RESUMED;
    }
    return boot_meta_save(m);
}

static uint8_t rollback_slot_to_app(boot_meta_t *m, uint16_t reason_evt)
{
    uint16_t total_pages;
    uint16_t i;

    /* 先确认 Slot 里的备份旧固件完好无损 */
    if (boot_verify_region(SB_SLOT_ADDR, m->slot_size, m->slot_crc32) != BOOT_OK) {
        return BOOT_ERR_NO_VALID_APP;
    }

    total_pages = (uint16_t)((m->slot_size + SB_FLASH_PAGE_SIZE - 1u) / SB_FLASH_PAGE_SIZE);
    for (i = 0u; i < total_pages; i++) {
        if (copy_page(SB_APP_ADDR + (uint32_t)i * SB_FLASH_PAGE_SIZE,
                      SB_SLOT_ADDR + (uint32_t)i * SB_FLASH_PAGE_SIZE) != BOOT_OK) {
            return BOOT_ERR_FLASH;
        }
    }

    m->app_version   = m->slot_version;
    m->app_size      = m->slot_size;
    m->app_crc32     = m->slot_crc32;
    m->state         = (uint16_t)BOOT_STATE_NORMAL;
    m->try_cnt       = 0u;
    m->swap_page_idx = 0u;
    m->last_event    = reason_evt;

    return boot_meta_save(m);
}

/* ======================= 上电状态机总入口 ======================= */

uint8_t boot_process_startup(boot_meta_t *m, boot_action_t *out_act)
{
    uint8_t did_install = 0u;

    *out_act = BOOT_ACT_STAY_BOOT;

    /* 1. 下载中途断电：不动原 App，直接按正常态尝试启动原 App */
    if (m->state == (uint16_t)BOOT_STATE_DOWNLOADING) {
        if (boot_verify_region(SB_APP_ADDR, m->app_size, m->app_crc32) == BOOT_OK) {
            *out_act = BOOT_ACT_JUMP_APP;
            return BOOT_OK;
        }
        return BOOT_ERR_NO_VALID_APP;
    }

    /* 2. 待安装（或上次搬运中途掉电）：校验 Slot（若第 0 页尚未建立 WAL 快照），然后逐页交换 */
    if (m->state == (uint16_t)BOOT_STATE_PENDING_INSTALL) {
        uint32_t tag_val = 0u;
        bsp_flash_read(WAL_SCRATCH_TAG_ADDR, (uint8_t *)&tag_val, (uint16_t)sizeof(tag_val));

        if (m->swap_page_idx == 0u && tag_val != (WAL_TAG_MAGIC | 0u)) {
            if (boot_verify_region(SB_SLOT_ADDR, m->slot_size, m->slot_crc32) != BOOT_OK) {
                /* Slot 校验不过，退回 NORMAL 跑旧 App */
                m->state = (uint16_t)BOOT_STATE_NORMAL;
                (void)boot_meta_save(m);
            } else {
                if (swap_app_and_slot(m) != BOOT_OK) {
                    return BOOT_ERR_FLASH;
                }
                did_install = 1u;
            }
        } else {
            /* 搬运中途掉电重启（含第 0 页 WAL 快照已建立但 swap_page_idx 尚未递增的场景）：继续交换 */
            if (swap_app_and_slot(m) != BOOT_OK) {
                return BOOT_ERR_FLASH;
            }
            did_install = 1u;
        }
    }

    /* 3. 试运行态（TESTING）：检查尝试次数是否超限 */
    if (m->state == (uint16_t)BOOT_STATE_TESTING) {
        if (m->try_cnt >= BOOT_META_MAX_TRY) {
            /* 连续 3 次启动均未 Commit 转正 -> 判定新固件死机，触发回滚 */
            m->state = (uint16_t)BOOT_STATE_ROLLBACK;
            (void)boot_meta_save(m);
        } else {
            m->try_cnt++;
            if (boot_meta_save(m) != BOOT_OK) {
                return BOOT_ERR_FLASH;
            }
            if (boot_verify_region(SB_APP_ADDR, m->app_size, m->app_crc32) == BOOT_OK) {
                *out_act = did_install ? BOOT_ACT_DID_INSTALL : BOOT_ACT_JUMP_APP;
                return BOOT_OK;
            }
            /* 新固件在 App 区居然 CRC 坏了，立刻转回滚 */
            m->state = (uint16_t)BOOT_STATE_ROLLBACK;
            (void)boot_meta_save(m);
        }
    }

    /* 4. 回滚态（ROLLBACK）：从 Slot 恢复旧好固件 */
    if (m->state == (uint16_t)BOOT_STATE_ROLLBACK) {
        if (rollback_slot_to_app(m, BOOT_EVT_ROLLBACK_WDOG) == BOOT_OK &&
            boot_verify_region(SB_APP_ADDR, m->app_size, m->app_crc32) == BOOT_OK) {
            *out_act = BOOT_ACT_DID_ROLLBACK;
            return BOOT_OK;
        }
        return BOOT_ERR_NO_VALID_APP;
    }

    /* 5. 正常态（NORMAL）：校验 App 区 CRC32，若损坏尝试从 Slot 自愈回滚 */
    if (boot_verify_region(SB_APP_ADDR, m->app_size, m->app_crc32) == BOOT_OK) {
        *out_act = BOOT_ACT_JUMP_APP;
        return BOOT_OK;
    }
    if (rollback_slot_to_app(m, BOOT_EVT_ROLLBACK_BAD_APP) == BOOT_OK &&
        boot_verify_region(SB_APP_ADDR, m->app_size, m->app_crc32) == BOOT_OK) {
        *out_act = BOOT_ACT_DID_ROLLBACK;
        return BOOT_OK;
    }

    return BOOT_ERR_NO_VALID_APP;
}

uint8_t boot_meta_commit_app(void)
{
    boot_meta_t m;
    uint8_t     from_bak = 0u;

    if (boot_meta_load(&m, &from_bak) != BOOT_OK) {
        return BOOT_ERR_CRC;
    }
    if (m.state == (uint16_t)BOOT_STATE_TESTING) {
        m.state      = (uint16_t)BOOT_STATE_NORMAL;
        m.try_cnt    = 0u;
        m.last_event = BOOT_EVT_UPGRADE_OK;
        return boot_meta_save(&m);
    }
    return BOOT_OK;
}
