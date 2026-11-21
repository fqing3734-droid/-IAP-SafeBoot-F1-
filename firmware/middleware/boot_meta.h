/**
 ******************************************************************************
 * @file    boot_meta.h
 * @brief   Boot Meta 双页持久化与防变砖五态状态机（含 A↔B 块交换备份与自动回滚）
 * @note    SafeBoot-F1
 *
 * 核心设计亮点：
 *   1. 双页先备后主：Meta 存于 0x0800E000（主）与 0x0800E400（备），每条 48 字节，
 *      带 magic("SBMT")、32 位递增 seq、layout_ver、CRC16-Modbus，任何时刻掉电不丢状态。
 *   2. 零额外空間的“页交换搬运（Swap-Install）”：
 *      安装新固件时，以 1KB 页为单位借助 RAM 缓冲将 App 区（旧好固件）与 Slot 区（新固件）
 *      逐页互换（并用 swap_progress 记录已交换页数，掉电重启可从断点继续交换！）。
 *      交换完成后：App 区是新固件，Slot 区自动变成上一版已知好固件（Golden Backup）！
 *   3. 看门狗试运行与 3 次失败自动回滚：
 *      新固件处于 STATE_TESTING，每次启动 try_cnt++；
 *      若新固件死锁/HardFault 未能调用 boot_meta_commit_app() 转正，
 *      连续 3 次（try_cnt > 3）后 Bootloader 自动把 Slot 里的旧好固件拷回 App 区并恢复运行！
 ******************************************************************************
 */
#ifndef __BOOT_META_H
#define __BOOT_META_H

#include "common.h"

/* ======================= 常量与魔数 ======================= */

#define BOOT_META_MAGIC             0x544D4253u     /* "SBMT" 小端 */
#define BOOT_META_LAYOUT_VER        1u
#define BOOT_META_MAX_TRY           3u              /* 试运行最多允许启动 3 次 */

/* ======================= 五态状态机 ======================= */

typedef enum {
    BOOT_STATE_NORMAL          = 0u,   /* 正常态：直接启动 App */
    BOOT_STATE_DOWNLOADING     = 1u,   /* 下载态：正在往 Slot 写新固件（支持断点续传） */
    BOOT_STATE_PENDING_INSTALL = 2u,   /* 待安装：Slot 已校验通过，等待与 App 逐页交换 */
    BOOT_STATE_TESTING         = 3u,   /* 试运行：新固件在 App 区试跑，满 3 次未 Commit 则回滚 */
    BOOT_STATE_ROLLBACK        = 4u    /* 回滚态：正在从 Slot 把旧版好固件刷回 App 区 */
} boot_state_t;

/* ======================= 事件/回滚原因码 ======================= */

#define BOOT_EVT_NONE               0u
#define BOOT_EVT_UPGRADE_OK         1u  /* 新固件已 Commit 转正 */
#define BOOT_EVT_ROLLBACK_WDOG      2u  /* 试运行超时/看门狗复位满 3 次触发自动回滚 */
#define BOOT_EVT_ROLLBACK_BAD_APP   3u  /* App 区 CRC32 或栈顶损坏触发回滚 */
#define BOOT_EVT_INSTALL_RESUMED    4u  /* 搬运中途掉电后重启续搬成功 */

/* ======================= 返回码 ======================= */

#define BOOT_OK                     0u
#define BOOT_ERR_CRC                1u
#define BOOT_ERR_MAGIC              2u
#define BOOT_ERR_BLANK              3u
#define BOOT_ERR_PARAM              4u
#define BOOT_ERR_FLASH              5u
#define BOOT_ERR_NO_VALID_APP       6u

/* ======================= 持久化数据结构（32 字节） ======================= */

typedef struct {
    uint16_t state;             /* boot_state_t */
    uint16_t try_cnt;           /* STATE_TESTING 下已尝试启动次数 */
    uint16_t swap_page_idx;     /* 逐页交换/回滚进度（0~24），用于搬运掉电续搬 */
    uint16_t last_event;        /* BOOT_EVT_*，供主站查询上次回滚/升级结果 */

    /* 当前 App 区固件描述 */
    uint32_t app_version;       /* 如 0x00010000 = v1.0 */
    uint32_t app_size;          /* 字节数（偶数，1 ~ 24576） */
    uint32_t app_crc32;         /* App 镜像 CRC32 */

    /* Slot 区固件描述（下载中为新固件；交换后为旧版备份固件） */
    uint32_t slot_version;
    uint32_t slot_size;
    uint32_t slot_crc32;
    uint16_t dl_done_seq;       /* 断点续传：已成功写入 Slot 的连续包数（0 = 尚未写） */
    uint16_t reserved;
} boot_meta_t;

/* 完整 Flash 记录：16B 头 + 36B 载荷 + 2B CRC16 + 2B pad = 56 字节 */
typedef struct {
    uint32_t    magic;          /* BOOT_META_MAGIC */
    uint32_t    seq;            /* 单调递增序号 */
    uint16_t    layout_ver;     /* BOOT_META_LAYOUT_VER */
    uint16_t    payload_size;   /* sizeof(boot_meta_t) = 36 */
    uint32_t    reserved_hdr;
    boot_meta_t meta;           /* 36 字节 */
    uint16_t    crc16;          /* 覆盖前 52 字节 */
    uint16_t    pad;            /* 0xFFFF */
} boot_meta_rec_t;

/* ======================= Bootloader 决策动作 ======================= */

typedef enum {
    BOOT_ACT_JUMP_APP       = 0u,   /* App 校验通过，直接跳转（若在 TESTING 则开看门狗） */
    BOOT_ACT_STAY_BOOT      = 1u,   /* 无有效 App 或被请求留在 Bootloader 等待串口升级 */
    BOOT_ACT_DID_INSTALL    = 2u,   /* 本次上电完成了新固件搬运并准备跳入试运行 */
    BOOT_ACT_DID_ROLLBACK   = 3u    /* 本次上电触发并完成了自动回滚，已恢复旧版好固件 */
} boot_action_t;

/* ======================= 接口 ======================= */

/** 初始化默认 Meta（空白芯片首次上电） */
void    boot_meta_defaults(boot_meta_t *m);

/** 从双页 Flash 加载最新有效 Meta（主坏回退备区，两区皆空则装默认值） */
uint8_t boot_meta_load(boot_meta_t *out, uint8_t *out_from_backup);

/** 按“先备区后主区 + 读回校验”保存 Meta */
uint8_t boot_meta_save(const boot_meta_t *m);

/** 读取当前 Flash 中的最新 seq */
uint32_t boot_meta_current_seq(void);

/** 校验指定分区（SB_APP_ADDR 或 SB_SLOT_ADDR）内的固件长度与 CRC32 是否匹配 */
uint8_t boot_verify_region(uint32_t base_addr, uint32_t size, uint32_t expect_crc32);

/**
 * @brief  Bootloader 上电核心决策与搬运/回滚状态机
 * @param  m         出入参：当前 Meta（内部若有状态流转会自动保存至 Flash）
 * @param  out_act   返回本次执行的动作（BOOT_ACT_*）
 * @return BOOT_OK = 可以跳转到 App；其它 = 必须留在 Bootloader 等待升级
 */
uint8_t boot_process_startup(boot_meta_t *m, boot_action_t *out_act);

/**
 * @brief  App 侧调用的“自我确认（Commit）”接口
 * @note   新固件启动进入主循环并完成自检后调用。若当前处于 STATE_TESTING，
 *         则将其转为 STATE_NORMAL、清零 try_cnt 并置 BOOT_EVT_UPGRADE_OK。
 */
uint8_t boot_meta_commit_app(void);

#endif /* __BOOT_META_H */
