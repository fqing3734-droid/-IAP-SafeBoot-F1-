/**
 ******************************************************************************
 * @file    iap_proto.c
 * @brief   SafeBoot-F1 二进制分包升级协议实现
 * @note    SafeBoot-F1
 ******************************************************************************
 */
#include "iap_proto.h"
#include "bsp_flash.h"
#include "crc16.h"
#include "crc32.h"
#include <string.h>

STATIC_ASSERT(sizeof(iap_query_rsp_t) == 24, iap_query_rsp_must_be_24);

/* ======================= 小端读写工具 ======================= */

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t build_rsp(uint8_t cmd,
                          uint16_t status_or_seq,
                          const uint8_t *payload,
                          uint16_t len,
                          uint8_t *out)
{
    uint16_t crc;

    out[0] = IAP_FRAME_HEAD0;
    out[1] = IAP_FRAME_HEAD1;
    out[2] = (uint8_t)(cmd | IAP_RSP_MASK);
    put_le16(&out[3], status_or_seq);
    put_le16(&out[5], len);
    if (len > 0u && payload != 0) {
        memcpy(&out[7], payload, len);
    }
    crc = crc16_modbus(out, (uint16_t)(7u + len));
    put_le16(&out[7u + len], crc);
    return (uint16_t)(9u + len);
}

/* ======================= 初始化 ======================= */

void iap_proto_init(iap_ctx_t *ctx, boot_meta_t *meta, uint8_t run_mode)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->meta         = meta;
    ctx->run_mode     = run_mode;
    ctx->ram_done_seq = (meta != 0) ? meta->dl_done_seq : 0u;
}

void iap_proto_reset_rx(iap_ctx_t *ctx)
{
    ctx->rx_len = 0u;
}

/* ======================= 各命令处理 ======================= */

/**
 * CMD_START 载荷（14 字节，小端）：
 *   [0..3]   new_version  (uint32)
 *   [4..7]   new_size     (uint32，偶数，1 ~ SB_SLOT_SIZE)
 *   [8..11]  new_crc32    (uint32)
 *   [12..13] flags        (uint16，bit0 = IAP_START_FLAG_RESUME)
 * 应答载荷（2 字节）：
 *   [0..1]   resume_seq   (uint16，上位机从此 seq 开始发 CMD_DATA)
 */
static uint16_t handle_start(iap_ctx_t *ctx, const uint8_t *pl, uint16_t len, uint8_t *rsp)
{
    uint32_t    ver, sz, crc;
    uint16_t    flags;
    uint16_t    resume_seq = 0u;
    uint8_t     ack_pl[2];
    boot_meta_t *m = ctx->meta;

    if (len < 14u) {
        return build_rsp(IAP_CMD_START, IAP_STATUS_ERR_PARAM, 0, 0u, rsp);
    }
    ver   = get_le32(&pl[0]);
    sz    = get_le32(&pl[4]);
    crc   = get_le32(&pl[8]);
    flags = get_le16(&pl[12]);

    if (sz == 0u || sz > SB_SLOT_SIZE || (sz & 1u) != 0u) {
        return build_rsp(IAP_CMD_START, IAP_STATUS_ERR_PARAM, 0, 0u, rsp);
    }

    /* 检查是否满足断点续传条件（同一份固件 + 已落盘整页进度 > 0） */
    if ((flags & IAP_START_FLAG_RESUME) != 0u &&
        m->state == (uint16_t)BOOT_STATE_DOWNLOADING &&
        m->slot_version == ver &&
        m->slot_size == sz &&
        m->slot_crc32 == crc &&
        m->dl_done_seq > 0u) {
        resume_seq        = m->dl_done_seq;
        ctx->ram_done_seq = resume_seq;
    } else {
        m->state        = (uint16_t)BOOT_STATE_DOWNLOADING;
        m->slot_version = ver;
        m->slot_size    = sz;
        m->slot_crc32   = crc;
        m->dl_done_seq  = 0u;
        ctx->ram_done_seq = 0u;
        if (boot_meta_save(m) != BOOT_OK) {
            return build_rsp(IAP_CMD_START, IAP_STATUS_ERR_FLASH, 0, 0u, rsp);
        }
    }

    put_le16(ack_pl, resume_seq);
    return build_rsp(IAP_CMD_START, IAP_STATUS_OK, ack_pl, 2u, rsp);
}

/**
 * CMD_DATA：
 *   seq = 包序号（0, 1, 2...）
 *   pl  = 数据块（2 ~ 128 字节，偶数）
 *   注：每当写入到达页首（offset % 1024 == 0）时自动擦除该页；
 *       每写满 1 页（8 包）或到达最后一包时，将 dl_done_seq 持久化到 Boot Meta，
 *       既保证掉电后能按页对齐断点续传，又避免每包都擦写 Meta 页。
 */
static uint16_t handle_data(iap_ctx_t *ctx,
                            uint16_t seq,
                            const uint8_t *pl,
                            uint16_t len,
                            uint8_t *rsp)
{
    boot_meta_t *m = ctx->meta;
    uint32_t    offset;
    uint32_t    dst_addr;
    uint8_t     ack_pl[2];

    if (m->state != (uint16_t)BOOT_STATE_DOWNLOADING) {
        return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_STATE, 0, 0u, rsp);
    }
    if (len == 0u || len > IAP_PKT_CHUNK_SIZE || (len & 1u) != 0u) {
        return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_PARAM, 0, 0u, rsp);
    }

    /* 1. 幂等重传：收到已经写过的旧包号，直接回 OK（不重复写 Flash） */
    if (seq < ctx->ram_done_seq) {
        put_le16(ack_pl, ctx->ram_done_seq);
        return build_rsp(IAP_CMD_DATA, IAP_STATUS_OK, ack_pl, 2u, rsp);
    }

    /* 2. 乱序/跳包：拒绝并返回当前期望的 ram_done_seq */
    if (seq > ctx->ram_done_seq) {
        put_le16(ack_pl, ctx->ram_done_seq);
        return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_SEQ, ack_pl, 2u, rsp);
    }

    /* 3. 正常顺序包（seq == ctx->ram_done_seq） */
    offset = (uint32_t)seq * IAP_PKT_CHUNK_SIZE;
    if (offset + len > m->slot_size) {
        return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_PARAM, 0, 0u, rsp);
    }
    dst_addr = SB_SLOT_ADDR + offset;

    /* 若正好在 1KB 页首，先擦除该页 */
    if ((offset & (SB_FLASH_PAGE_SIZE - 1u)) == 0u) {
        if (bsp_flash_erase_page(dst_addr) != BSP_FLASH_OK) {
            return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_FLASH, 0, 0u, rsp);
        }
    }

    if (bsp_flash_program(dst_addr, pl, len) != BSP_FLASH_OK) {
        return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_FLASH, 0, 0u, rsp);
    }

    ctx->ram_done_seq = (uint16_t)(seq + 1u);

    /* 每满 1 页（8 包 = 1024B）将该页断点落盘到 Meta，掉电重连直接从该页首续传 */
    if ((ctx->ram_done_seq % IAP_PKTS_PER_PAGE) == 0u) {
        m->dl_done_seq = ctx->ram_done_seq;
        if (boot_meta_save(m) != BOOT_OK) {
            return build_rsp(IAP_CMD_DATA, IAP_STATUS_ERR_FLASH, 0, 0u, rsp);
        }
    }

    put_le16(ack_pl, ctx->ram_done_seq);
    return build_rsp(IAP_CMD_DATA, IAP_STATUS_OK, ack_pl, 2u, rsp);
}

/**
 * CMD_END：校验整包长度与 Slot 区 CRC32，通过则切换为 STATE_PENDING_INSTALL 并请求重启
 */
static uint16_t handle_end(iap_ctx_t *ctx, uint8_t *rsp)
{
    boot_meta_t *m = ctx->meta;
    uint16_t    expect_pkts;

    if (m->state != (uint16_t)BOOT_STATE_DOWNLOADING || m->slot_size == 0u) {
        return build_rsp(IAP_CMD_END, IAP_STATUS_ERR_STATE, 0, 0u, rsp);
    }
    expect_pkts = (uint16_t)((m->slot_size + IAP_PKT_CHUNK_SIZE - 1u) / IAP_PKT_CHUNK_SIZE);
    if (ctx->ram_done_seq != expect_pkts) {
        return build_rsp(IAP_CMD_END, IAP_STATUS_ERR_SEQ, 0, 0u, rsp);
    }

    /* 整包 CRC32 校验 */
    if (boot_verify_region(SB_SLOT_ADDR, m->slot_size, m->slot_crc32) != BOOT_OK) {
        return build_rsp(IAP_CMD_END, IAP_STATUS_ERR_CRC, 0, 0u, rsp);
    }

    m->state         = (uint16_t)BOOT_STATE_PENDING_INSTALL;
    m->swap_page_idx = 0u;
    m->dl_done_seq   = ctx->ram_done_seq;
    if (boot_meta_save(m) != BOOT_OK) {
        return build_rsp(IAP_CMD_END, IAP_STATUS_ERR_FLASH, 0, 0u, rsp);
    }

    ctx->reboot_requested = 1u;
    return build_rsp(IAP_CMD_END, IAP_STATUS_OK, 0, 0u, rsp);
}

static uint16_t handle_query(iap_ctx_t *ctx, uint8_t *rsp)
{
    iap_query_rsp_t q;
    boot_meta_t     *m = ctx->meta;

    memset(&q, 0, sizeof(q));
    q.run_mode     = ctx->run_mode;
    q.state        = (uint8_t)m->state;
    q.try_cnt      = (uint8_t)m->try_cnt;
    q.last_event   = (uint8_t)m->last_event;
    q.dl_done_seq  = m->dl_done_seq;
    q.app_version  = m->app_version;
    q.app_size     = m->app_size;
    q.app_crc32    = m->app_crc32;
    q.slot_version = m->slot_version;

    return build_rsp(IAP_CMD_QUERY, IAP_STATUS_OK, (const uint8_t *)&q, (uint16_t)sizeof(q), rsp);
}

/* ======================= 帧入口 ======================= */

uint16_t iap_proto_handle_frame(iap_ctx_t *ctx,
                                const uint8_t *req,
                                uint16_t req_len,
                                uint8_t *rsp)
{
    uint8_t  cmd;
    uint16_t seq, len, got_crc, calc_crc;

    if (req_len < 9u || req_len > IAP_MAX_FRAME_LEN) {
        return 0u;
    }
    if (req[0] != IAP_FRAME_HEAD0 || req[1] != IAP_FRAME_HEAD1) {
        return 0u;
    }
    cmd = req[2];
    seq = get_le16(&req[3]);
    len = get_le16(&req[5]);
    if (len > IAP_MAX_PAYLOAD || (uint16_t)(9u + len) != req_len) {
        return 0u;
    }

    got_crc  = get_le16(&req[7u + len]);
    calc_crc = crc16_modbus(req, (uint16_t)(7u + len));
    if (got_crc != calc_crc) {
        /* 帧级 CRC16 错：静默丢弃，由上位机超时重传 */
        return 0u;
    }

    switch (cmd) {
    case IAP_CMD_START:
        return handle_start(ctx, &req[7], len, rsp);
    case IAP_CMD_DATA:
        return handle_data(ctx, seq, &req[7], len, rsp);
    case IAP_CMD_END:
        return handle_end(ctx, rsp);
    case IAP_CMD_QUERY:
        return handle_query(ctx, rsp);
    case IAP_CMD_REBOOT:
        ctx->reboot_requested = 1u;
        return build_rsp(IAP_CMD_REBOOT, IAP_STATUS_OK, 0, 0u, rsp);
    default:
        return build_rsp(cmd, IAP_STATUS_ERR_PARAM, 0, 0u, rsp);
    }
}

uint16_t iap_proto_feed_byte(iap_ctx_t *ctx, uint8_t byte, uint8_t *rsp)
{
    uint16_t pay_len;
    uint16_t total_need;
    uint16_t out_len;

    if (ctx->rx_len == 0u) {
        if (byte == IAP_FRAME_HEAD0) {
            ctx->rx_buf[ctx->rx_len++] = byte;
        }
        return 0u;
    }
    if (ctx->rx_len == 1u) {
        if (byte == IAP_FRAME_HEAD1) {
            ctx->rx_buf[ctx->rx_len++] = byte;
        } else if (byte == IAP_FRAME_HEAD0) {
            ctx->rx_buf[0] = IAP_FRAME_HEAD0;
            ctx->rx_len = 1u;
        } else {
            ctx->rx_len = 0u;
        }
        return 0u;
    }

    ctx->rx_buf[ctx->rx_len++] = byte;

    if (ctx->rx_len >= 7u) {
        pay_len = get_le16(&ctx->rx_buf[5]);
        if (pay_len > IAP_MAX_PAYLOAD) {
            ctx->rx_len = 0u;
            return 0u;
        }
        total_need = (uint16_t)(9u + pay_len);
        if (ctx->rx_len >= total_need) {
            out_len = iap_proto_handle_frame(ctx, ctx->rx_buf, total_need, rsp);
            ctx->rx_len = 0u;
            return out_len;
        }
    }
    return 0u;
}
