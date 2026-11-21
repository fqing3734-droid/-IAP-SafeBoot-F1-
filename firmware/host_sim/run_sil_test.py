#!/usr/bin/env python3
"""
SafeBoot-F1 端到端 SIL（软件在环）自动化对打测试套件

真实启动 safeboot_sim.exe（链接真实 boot_meta.c + iap_proto.c + crc16.c + crc32.c），
通过二进制串口管道与 upgrader.py 全流程对打，验证以下 4 个核心工业场景：
  [Case 1] 空白芯片首次升级 App v1.0 -> 安装 -> Commit 转正 -> NORMAL 运行
  [Case 2] 运行态升级 v1.0 -> v1.1（App 软复位切 Bootloader -> WAL 交换 -> 转正，Slot 保留 v1.0）
  [Case 3] 下载中途断链/掉电 -> 1KB 页对齐断点续传升级到 v1.2（验证从 seq=8 续传而非 seq=0）
  [Case 4] 刷入死锁故障固件 v1.3-BAD -> 连续 3 次看门狗复位 -> 自动回滚至 v1.2 并上报 ROLLBACK_WDOG
"""

import os
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "tools"))
sys.path.insert(0, TOOLS_DIR)

from upgrader import (  # noqa: E402
    EVT_NAMES,
    STATE_NAMES,
    SafeBootClient,
    SimPipeTransport,
    crc32_ieee,
    make_sim_firmware,
)

SIM_EXE = os.path.join(SCRIPT_DIR, "safeboot_sim.exe")


def run_all() -> None:
    if not os.path.exists(SIM_EXE):
        raise SystemExit(f"找不到 {SIM_EXE}，请先运行 host_sim/build.ps1 编译")

    flash_file = os.path.join(tempfile.gettempdir(), "safeboot_sil_flash.bin")
    if os.path.exists(flash_file):
        os.remove(flash_file)

    fw_v10 = make_sim_firmware(0x00010000, size=3072, deadlock=False)  # 3KB (24 pkts)
    fw_v11 = make_sim_firmware(0x00010001, size=4352, deadlock=False)  # 4.25KB (34 pkts)
    fw_v12 = make_sim_firmware(0x00010002, size=3584, deadlock=False)  # 3.5KB (28 pkts)
    fw_bad = make_sim_firmware(0x00010003, size=2560, deadlock=True)   # 2.5KB (20 pkts, DEADLOCK!)

    print("=" * 64)
    print("SafeBoot-F1 SIL 端到端软件在环对打测试")
    print("=" * 64)

    # ---------- Case 1: 空白芯片首次刷入 v1.0 ----------
    print("\n[Case 1] 空白芯片首次升级 App v1.0 -> 安装 -> Commit 转正")
    tr = SimPipeTransport(SIM_EXE, flash_path=flash_file, boot_wait_ms=150, verbose_sim=True)
    try:
        cli = SafeBootClient(tr, verbose=True)
        q0 = cli.query()
        assert q0.run_mode == 0, f"空白芯片应驻留 Bootloader，实际 run_mode={q0.run_mode}"
        assert q0.app_size == 0

        q1 = cli.upgrade(fw_v10, version=0x00010000, wait_boot_s=0.1)
        assert q1 is not None
        assert q1.run_mode == 1, "v1.0 应处于 APP 模式"
        assert q1.state == 0, f"v1.0 应已 Commit 为 NORMAL，实际 {STATE_NAMES.get(q1.state)}"
        assert q1.app_version == 0x00010000
        assert q1.app_crc32 == crc32_ieee(fw_v10)
        assert q1.last_event == 1, f"期望 UPGRADE_OK(1)，实际 {EVT_NAMES.get(q1.last_event)}"
        print("  ==> Case 1 PASS!")
    finally:
        tr.close()

    # ---------- Case 2: 正常升级 v1.0 -> v1.1 ----------
    print("\n[Case 2] 运行态升级 v1.0 -> v1.1（App 软复位切 Boot + WAL 互换 + Golden 备份）")
    tr = SimPipeTransport(SIM_EXE, flash_path=flash_file, boot_wait_ms=80, verbose_sim=True)
    try:
        cli = SafeBootClient(tr, verbose=True)
        # 等 120ms 让仿真器超时握手窗口直接启动 v1.0 App
        import time
        time.sleep(0.15)
        q_before = cli.query()
        assert q_before.run_mode == 1 and q_before.app_version == 0x00010000

        q2 = cli.upgrade(fw_v11, version=0x00010001, wait_boot_s=0.1)
        assert q2 is not None
        assert q2.run_mode == 1
        assert q2.state == 0
        assert q2.app_version == 0x00010001
        assert q2.app_crc32 == crc32_ieee(fw_v11)
        assert q2.slot_version == 0x00010000, "Slot 必须自动保留旧版 v1.0 作为回滚备份"
        assert q2.last_event == 1
        print("  ==> Case 2 PASS!")
    finally:
        tr.close()

    # ---------- Case 3: 下载中途断链 + 1KB 页对齐断点续传 v1.1 -> v1.2 ----------
    print("\n[Case 3] 升级 v1.1 -> v1.2 发送 11 包后强行断链 -> 重启续传（应从 seq=8 续传）")
    # 3a. 第一段连接：发送 11 包后断开（第 0 页 8 包已落盘，第 1 页仅写 3 包）
    tr = SimPipeTransport(SIM_EXE, flash_path=flash_file, boot_wait_ms=200, verbose_sim=True)
    try:
        cli = SafeBootClient(tr, verbose=True)
        res = cli.upgrade(fw_v12, version=0x00010002, resume=True, drop_after_pkts=11)
        assert res is None
    finally:
        tr.close()

    # 3b. 断电重启后再连：此时原 App v1.1 仍完好运行！切回 Bootloader 后带 RESUME 续传
    tr = SimPipeTransport(SIM_EXE, flash_path=flash_file, boot_wait_ms=200, verbose_sim=True)
    try:
        cli = SafeBootClient(tr, verbose=True)
        info_boot = cli.enter_bootloader()
        assert info_boot.state == 1, "断点重启后 Meta 状态应为 DOWNLOADING"
        assert info_boot.dl_done_seq == 8, f"页边界断点应为 8，实际 {info_boot.dl_done_seq}"

        # 直接验证 START 返回 resume_seq == 8，然后走完完整 upgrade
        resume_seq = cli.start(0x00010002, len(fw_v12), crc32_ieee(fw_v12), resume=True)
        assert resume_seq == 8, f"期望续传 seq=8，实际 {resume_seq}"

        q3 = cli.upgrade(fw_v12, version=0x00010002, resume=True, wait_boot_s=0.1)
        assert q3 is not None
        assert q3.run_mode == 1
        assert q3.state == 0
        assert q3.app_version == 0x00010002
        assert q3.app_crc32 == crc32_ieee(fw_v12)
        assert q3.slot_version == 0x00010001
        print("  ==> Case 3 PASS!")
    finally:
        tr.close()

    # ---------- Case 4: 刷入死锁坏固件 v1.3-BAD -> 3 次看门狗复位自动回滚至 v1.2 ----------
    print("\n[Case 4] 刷入死锁故障固件 v1.3-BAD -> 3 次 IWDG 复位自动回滚至 v1.2")
    tr = SimPipeTransport(SIM_EXE, flash_path=flash_file, boot_wait_ms=200, verbose_sim=True)
    try:
        cli = SafeBootClient(tr, verbose=True)
        q4 = cli.upgrade(fw_bad, version=0x00010003, resume=True, wait_boot_s=0.1)
        assert q4 is not None
        assert q4.run_mode == 1, "回滚后应正常运行旧版 App"
        assert q4.state == 0, f"回滚后状态应恢复 NORMAL，实际 {STATE_NAMES.get(q4.state)}"
        assert q4.app_version == 0x00010002, f"必须自动回滚回 v1.2，实际 0x{q4.app_version:08X}"
        assert q4.app_crc32 == crc32_ieee(fw_v12), "回滚后的 App CRC32 必须与 v1.2 逐字节一致"
        assert q4.last_event == 2, f"last_event 必须为 ROLLBACK_WDOG(2)，实际 {EVT_NAMES.get(q4.last_event)}"
        print("  ==> Case 4 PASS! 故障固件已自动回滚至 v1.2！")
    finally:
        tr.close()

    print("\n" + "=" * 64)
    print("ALL 4 SIL END-TO-END SCENARIOS PASSED!")
    print("=" * 64)


if __name__ == "__main__":
    run_all()
