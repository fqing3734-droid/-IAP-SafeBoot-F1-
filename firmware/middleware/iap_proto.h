/**
 ******************************************************************************
 * @file    iap_proto.h
 * @brief   SafeBoot-F1 二进制分包升级协议（支持幂等重传、1KB 页边界断点续传、整包 CRC32）
 * @note    SafeBoot-F1
 *
 * 帧格式（小端）：
 *   [0]     0xAA            帧头高字节
 *   [1]     0x55            帧头低字节
 *   [2]     cmd             命令字（请求 0x01~0x05；应答 0x80 | cmd）
 *   [3..4]  seq             包序号 uint16（DATA 命令为 0,1,2...；其它命令为状态码）
 *   [5..6]  len             载荷长度 uint16（0 ~ 128）
 *   [7..7+len-1] payload    载荷字节
 *   [7+len..8+len] crc16    前 (7 + len) 字节的 CRC16-Modbus（低字节在前）
 ******************************************************************************
 */
#ifndef __IAP_PROTO_H
#define __IAP_PROTO_H

#include "common.h"
#include "boot_meta.h"

/* ======================= 帧常量 ======================= */

#define IAP_FRAME_HEAD0         0xAAu
#define IAP_FRAME_HEAD1         0x55u
#define IAP_PKT_CHUNK_SIZE      128u        /* 每个 DATA 包固定 128 字节（末包可小于 128） */
#define IAP_PKTS_PER_PAGE       (SB_FLASH_PAGE_SIZE / IAP_PKT_CHUNK_SIZE) /* 8 包 = 1 页(1KB) */
#define IAP_MAX_PAYLOAD         136u
#define IAP_MAX_FRAME_LEN       (7u + IAP_MAX_PAYLOAD + 2u)

/* ======================= 命令字 ======================= */

#define IAP_CMD_START           0x01u       /* 开始/续传升级：下发 version/size/crc32/flags */
#define IAP_CMD_DATA            0x02u       /* 下发 128B 数据块 */
#define IAP_CMD_END             0x03u       /* 结束下载：触发整包 CRC32 校验并置 PENDING_INSTALL */
#define IAP_CMD_QUERY           0x04u       /* 查询状态、版本、回滚原因、断点包号 */
#define IAP_CMD_REBOOT          0x05u       /* 请求软复位（App 收到后切回 Bootloader） */

#define IAP_RSP_MASK            0x80u

/* ======================= 应答状态码（放在应答帧的 seq 字段） ======================= */

#define IAP_STATUS_OK           0x0000u
#define IAP_STATUS_ERR_CRC      0x0001u     /* 帧 CRC16 或整包 CRC32 错误 */
#define IAP_STATUS_ERR_SEQ      0x0002u     /* 包序号不连续（应答 payload 返回期望的 dl_done_seq） */
#define IAP_STATUS_ERR_PARAM    0x0003u     /* 固件大小越界或参数非法 */
#define IAP_STATUS_ERR_FLASH    0x0004u     /* Flash 擦写失败 */
#define IAP_STATUS_ERR_STATE    0x0005u     /* 当前未处于 DOWNLOADING 状态 */

/* START 命令 flags 位 */
#define IAP_START_FLAG_RESUME   0x0001u     /* 1 = 若 Slot 中已有同一固件则从断点页续传 */

/* ======================= QUERY 应答载荷（24 字节，小端） ======================= */

typedef struct {
    uint8_t  run_mode;          /* 0 = Bootloader, 1 = App */
    uint8_t  state;             /* boot_state_t */
    uint8_t  try_cnt;           /* 当前试运行计数 */
    uint8_t  last_event;        /* BOOT_EVT_* */
    uint16_t dl_done_seq;       /* 已落盘的安全续传包序号（8 的倍数） */
    uint16_t reserved;
    uint32_t app_version;
    uint32_t app_size;
    uint32_t app_crc32;
    uint32_t slot_version;
} iap_query_rsp_t;

/* ======================= 协议上下文 ======================= */

typedef struct {
    boot_meta_t *meta;
    uint8_t      run_mode;          /* 0 = Bootloader, 1 = App */
    uint8_t      reboot_requested;  /* 1 = 应答发完后主循环应执行 NVIC_SystemReset */
    uint16_t     ram_done_seq;      /* 当前会话已写入 Flash 的包序号 */
    uint8_t      rx_buf[IAP_MAX_FRAME_LEN];
    uint16_t     rx_len;
} iap_ctx_t;

/* ======================= 接口 ======================= */

void     iap_proto_init(iap_ctx_t *ctx, boot_meta_t *meta, uint8_t run_mode);
void     iap_proto_reset_rx(iap_ctx_t *ctx);

/**
 * @brief  处理一帧完整的请求报文并生成应答帧
 * @param  ctx      协议上下文
 * @param  req      输入帧（含 AA 55 头与末尾 CRC16）
 * @param  req_len  输入帧字节数
 * @param  rsp      输出应答帧缓冲（至少 IAP_MAX_FRAME_LEN 字节）
 * @return 应答帧长度（0 = 帧头/长度/CRC16 错误，静默丢弃）
 */
uint16_t iap_proto_handle_frame(iap_ctx_t *ctx,
                                const uint8_t *req,
                                uint16_t req_len,
                                uint8_t *rsp);

/**
 * @brief  流式喂入串口收到的单字节；当凑齐一帧时自动调用 iap_proto_handle_frame
 * @return >0 表示已生成应答帧（长度=返回值，内容在 rsp）；0 表示尚未凑齐或坏帧已丢弃
 */
uint16_t iap_proto_feed_byte(iap_ctx_t *ctx, uint8_t byte, uint8_t *rsp);

#endif /* __IAP_PROTO_H */
