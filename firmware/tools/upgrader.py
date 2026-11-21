#!/usr/bin/env python3
r"""
SafeBoot-F1 上位机升级器（支持真实串口 COMx 与 PC 端 SIL 管道仿真器双模式）

协议帧格式（小端，与 firmware/middleware/iap_proto.h 严格对齐）：
  [0]        0xAA
  [1]        0x55
  [2]        cmd            请求 0x01~0x05；应答 0x80 | cmd
  [3..4]     seq / status   uint16
  [5..6]     len            uint16 (0 ~ 136)
  [7..7+L-1] payload
  [7+L..8+L] crc16          CRC16-Modbus（前 7+L 字节，低字节在前）

用法示例：
  1) 真板查询状态：
     python upgrader.py --port COM3 --baud 9600 query
  2) 真板升级固件：
     python upgrader.py --port COM3 --baud 9600 upgrade ..\MDK\Objects\SafeBoot_App_v11.bin --version 1.1
  3) SIL 仿真器升级：
     python upgrader.py --sim ..\host_sim\safeboot_sim.exe --flash sim.bin upgrade fw.bin --version 1.1
"""

import argparse
import os
import queue
import struct
import subprocess
import sys
import threading
import time
import zlib
from dataclasses import dataclass
from typing import List, Optional, Tuple

# ======================= 协议常量 =======================

FRAME_HEAD0 = 0xAA
FRAME_HEAD1 = 0x55
PKT_CHUNK_SIZE = 128
MAX_PAYLOAD = 136

CMD_START = 0x01
CMD_DATA = 0x02
CMD_END = 0x03
CMD_QUERY = 0x04
CMD_REBOOT = 0x05
RSP_MASK = 0x80

STATUS_OK = 0x0000
STATUS_ERR_CRC = 0x0001
STATUS_ERR_SEQ = 0x0002
STATUS_ERR_PARAM = 0x0003
STATUS_ERR_FLASH = 0x0004
STATUS_ERR_STATE = 0x0005

STATUS_NAMES = {
    STATUS_OK: "OK",
    STATUS_ERR_CRC: "ERR_CRC",
    STATUS_ERR_SEQ: "ERR_SEQ",
    STATUS_ERR_PARAM: "ERR_PARAM",
    STATUS_ERR_FLASH: "ERR_FLASH",
    STATUS_ERR_STATE: "ERR_STATE",
}

START_FLAG_RESUME = 0x0001

STATE_NAMES = {
    0: "NORMAL",
    1: "DOWNLOADING",
    2: "PENDING_INSTALL",
    3: "TESTING",
    4: "ROLLBACK",
}

EVT_NAMES = {
    0: "NONE",
    1: "UPGRADE_OK",
    2: "ROLLBACK_WDOG",
    3: "ROLLBACK_BAD_APP",
    4: "INSTALL_RESUMED",
}

# ======================= CRC 工具 =======================


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def crc32_ieee(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def parse_version(ver_str: str) -> int:
    """支持 '1.1' -> 0x00010001 或 '0x00010001'"""
    s = ver_str.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    if "." in s:
        parts = s.split(".")
        if len(parts) != 2:
            raise ValueError(f"版本号格式应为 major.minor 或 0xXXXXXXXX，收到: {ver_str}")
        major, minor = int(parts[0]), int(parts[1])
        return ((major & 0xFFFF) << 16) | (minor & 0xFFFF)
    return int(s, 0)


def fmt_version(ver: int) -> str:
    return f"v{(ver >> 16) & 0xFFFF}.{ver & 0xFFFF} (0x{ver:08X})"


def build_frame(cmd: int, seq: int = 0, payload: bytes = b"") -> bytes:
    hdr = struct.pack("<BBBHH", FRAME_HEAD0, FRAME_HEAD1, cmd, seq, len(payload))
    body = hdr + payload
    crc = crc16_modbus(body)
    return body + struct.pack("<H", crc)


# ======================= 数据结构 =======================


@dataclass
class QueryInfo:
    run_mode: int
    state: int
    try_cnt: int
    last_event: int
    dl_done_seq: int
    app_version: int
    app_size: int
    app_crc32: int
    slot_version: int

    @classmethod
    def from_payload(cls, pl: bytes) -> "QueryInfo":
        if len(pl) != 24:
            raise ValueError(f"QUERY payload length mismatch: {len(pl)}")
        (
            run_mode,
            state,
            try_cnt,
            last_event,
            dl_done_seq,
            _rsv,
            app_ver,
            app_sz,
            app_crc,
            slot_ver,
        ) = struct.unpack("<BBBBHHIIII", pl)
        return cls(
            run_mode=run_mode,
            state=state,
            try_cnt=try_cnt,
            last_event=last_event,
            dl_done_seq=dl_done_seq,
            app_version=app_ver,
            app_size=app_sz,
            app_crc32=app_crc,
            slot_version=slot_ver,
        )

    def pretty(self) -> str:
        mode = "APP" if self.run_mode else "BOOTLOADER"
        st = STATE_NAMES.get(self.state, f"STATE({self.state})")
        evt = EVT_NAMES.get(self.last_event, f"EVT({self.last_event})")
        return (
            f"mode={mode} state={st} try={self.try_cnt} last_evt={evt} "
            f"dl_seq={self.dl_done_seq} app={fmt_version(self.app_version)} "
            f"size={self.app_size}B crc=0x{self.app_crc32:08X} "
            f"slot={fmt_version(self.slot_version)}"
        )


# ======================= 传输层抽象 =======================


class BaseTransport:
    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def read_byte(self, timeout_s: float) -> Optional[int]:
        raise NotImplementedError

    def flush_rx(self) -> None:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class _Win32Serial:
    """无 pyserial 时的零依赖 Win32 原生串口实现（仅用标准库 ctypes）"""

    def __init__(self, port: str, baud: int = 9600):
        import ctypes
        from ctypes import wintypes

        self._ctypes = ctypes
        self._k32 = ctypes.WinDLL("kernel32", use_last_error=True)

        class DCB(ctypes.Structure):
            _fields_ = [
                ("DCBlength", wintypes.DWORD),
                ("BaudRate", wintypes.DWORD),
                ("fBitFields", wintypes.DWORD),
                ("wReserved", wintypes.WORD),
                ("XonLim", wintypes.WORD),
                ("XoffLim", wintypes.WORD),
                ("ByteSize", wintypes.BYTE),
                ("Parity", wintypes.BYTE),
                ("StopBits", wintypes.BYTE),
                ("XonChar", ctypes.c_char),
                ("XoffChar", ctypes.c_char),
                ("ErrorChar", ctypes.c_char),
                ("EofChar", ctypes.c_char),
                ("EvtChar", ctypes.c_char),
                ("wReserved1", wintypes.WORD),
            ]

        class COMMTIMEOUTS(ctypes.Structure):
            _fields_ = [
                ("ReadIntervalTimeout", wintypes.DWORD),
                ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
                ("ReadTotalTimeoutConstant", wintypes.DWORD),
                ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
                ("WriteTotalTimeoutConstant", wintypes.DWORD),
            ]

        self._COMMTIMEOUTS = COMMTIMEOUTS
        self._wintypes = wintypes
        self._cur_timeout_ms = -1

        dev = port if port.startswith(r"\\.") else rf"\\.\{port}"
        GENERIC_READ = 0x80000000
        GENERIC_WRITE = 0x40000000
        OPEN_EXISTING = 3
        INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

        self._k32.CreateFileW.restype = wintypes.HANDLE
        self._h = self._k32.CreateFileW(
            dev, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None
        )
        if self._h == INVALID_HANDLE_VALUE or not self._h:
            err = ctypes.get_last_error()
            raise OSError(f"无法打开串口 {port} (WinError {err})")

        self._k32.SetupComm(self._h, 4096, 4096)

        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not self._k32.GetCommState(self._h, ctypes.byref(dcb)):
            self.close()
            raise OSError(f"GetCommState 失败 (WinError {ctypes.get_last_error()})")
        dcb.BaudRate = baud
        dcb.ByteSize = 8
        dcb.Parity = 0      # NOPARITY
        dcb.StopBits = 0    # ONESTOPBIT
        dcb.fBitFields = 0x00000001  # fBinary=1，禁用流控
        if not self._k32.SetCommState(self._h, ctypes.byref(dcb)):
            self.close()
            raise OSError(f"SetCommState 失败 (WinError {ctypes.get_last_error()})")

        self._set_timeout_ms(50)
        self.reset_input_buffer()

    def _set_timeout_ms(self, ms: int) -> None:
        ms = max(10, int(ms))
        if ms == self._cur_timeout_ms:
            return
        to = self._COMMTIMEOUTS(0, 0, ms, 0, 1000)
        self._k32.SetCommTimeouts(self._h, self._ctypes.byref(to))
        self._cur_timeout_ms = ms

    def write(self, data: bytes) -> None:
        written = self._wintypes.DWORD(0)
        if not self._k32.WriteFile(
            self._h, data, len(data), self._ctypes.byref(written), None
        ):
            raise OSError(f"串口写失败 (WinError {self._ctypes.get_last_error()})")

    def read_byte(self, timeout_s: float) -> Optional[int]:
        self._set_timeout_ms(int(timeout_s * 1000))
        buf = self._ctypes.create_string_buffer(1)
        nread = self._wintypes.DWORD(0)
        if not self._k32.ReadFile(self._h, buf, 1, self._ctypes.byref(nread), None):
            raise OSError(f"串口读失败 (WinError {self._ctypes.get_last_error()})")
        return buf.raw[0] if nread.value == 1 else None

    def reset_input_buffer(self) -> None:
        PURGE_RXCLEAR = 0x0008
        PURGE_RXABORT = 0x0002
        self._k32.PurgeComm(self._h, PURGE_RXCLEAR | PURGE_RXABORT)

    def close(self) -> None:
        if getattr(self, "_h", None):
            self._k32.CloseHandle(self._h)
            self._h = None


class SerialTransport(BaseTransport):
    def __init__(self, port: str, baud: int = 9600):
        try:
            import serial  # type: ignore

            self.ser = serial.Serial(port=port, baudrate=baud, timeout=0.05)
            self._native = None
        except ModuleNotFoundError:
            self.ser = None
            self._native = _Win32Serial(port=port, baud=baud)

    def write(self, data: bytes) -> None:
        if self.ser is not None:
            self.ser.write(data)
            self.ser.flush()
        else:
            assert self._native is not None
            self._native.write(data)

    def read_byte(self, timeout_s: float) -> Optional[int]:
        if self.ser is not None:
            self.ser.timeout = max(0.01, timeout_s)
            b = self.ser.read(1)
            return b[0] if b else None
        assert self._native is not None
        return self._native.read_byte(timeout_s)

    def flush_rx(self) -> None:
        if self.ser is not None:
            self.ser.reset_input_buffer()
        else:
            assert self._native is not None
            self._native.reset_input_buffer()

    def close(self) -> None:
        if self.ser is not None and self.ser.is_open:
            self.ser.close()
        elif self._native is not None:
            self._native.close()


class SimPipeTransport(BaseTransport):
    """通过二进制 stdio 管道连接 PC 端 safeboot_sim.exe"""

    def __init__(
        self,
        sim_exe: str,
        flash_path: Optional[str] = None,
        boot_wait_ms: int = 200,
        stay_boot: bool = False,
        verbose_sim: bool = False,
    ):
        cmd = [sim_exe, "--boot-wait-ms", str(boot_wait_ms)]
        if flash_path:
            cmd += ["--flash", flash_path]
        if stay_boot:
            cmd += ["--stay-boot"]

        self.proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        self._rx_q: "queue.Queue[Optional[int]]" = queue.Queue()
        self.sim_logs: List[str] = []
        self._verbose_sim = verbose_sim

        self._t_out = threading.Thread(target=self._reader_stdout, daemon=True)
        self._t_err = threading.Thread(target=self._reader_stderr, daemon=True)
        self._t_out.start()
        self._t_err.start()

    def _reader_stdout(self) -> None:
        assert self.proc.stdout is not None
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                self._rx_q.put(None)
                break
            self._rx_q.put(chunk[0])

    def _reader_stderr(self) -> None:
        assert self.proc.stderr is not None
        for raw in self.proc.stderr:
            line = raw.decode("utf-8", errors="replace").rstrip()
            self.sim_logs.append(line)
            if self._verbose_sim:
                print(f"  {line}", file=sys.stderr)

    def write(self, data: bytes) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def read_byte(self, timeout_s: float) -> Optional[int]:
        try:
            b = self._rx_q.get(timeout=max(0.005, timeout_s))
            return b
        except queue.Empty:
            return None

    def flush_rx(self) -> None:
        while True:
            try:
                self._rx_q.get_nowait()
            except queue.Empty:
                break

    def close(self) -> None:
        if self.proc:
            try:
                if self.proc.stdin:
                    self.proc.stdin.close()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=1.5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=1.0)


# ======================= 协议客户端 =======================


class IAPError(RuntimeError):
    pass


class SafeBootClient:
    def __init__(self, transport: BaseTransport, verbose: bool = True):
        self.tr = transport
        self.verbose = verbose

    def _recv_frame(self, timeout_s: float = 1.0) -> Optional[Tuple[int, int, bytes]]:
        deadline = time.monotonic() + timeout_s
        buf = bytearray()

        while True:
            rem = deadline - time.monotonic()
            if rem <= 0:
                return None
            b = self.tr.read_byte(rem)
            if b is None:
                return None

            if len(buf) == 0:
                if b == FRAME_HEAD0:
                    buf.append(b)
                continue
            if len(buf) == 1:
                if b == FRAME_HEAD1:
                    buf.append(b)
                elif b == FRAME_HEAD0:
                    buf[:] = bytes([FRAME_HEAD0])
                else:
                    buf.clear()
                continue

            buf.append(b)
            if len(buf) >= 7:
                pay_len = struct.unpack_from("<H", buf, 5)[0]
                if pay_len > MAX_PAYLOAD:
                    buf.clear()
                    continue
                need = 9 + pay_len
                if len(buf) >= need:
                    got_crc = struct.unpack_from("<H", buf, 7 + pay_len)[0]
                    calc_crc = crc16_modbus(bytes(buf[: 7 + pay_len]))
                    if got_crc == calc_crc:
                        cmd = buf[2]
                        status_or_seq = struct.unpack_from("<H", buf, 3)[0]
                        payload = bytes(buf[7 : 7 + pay_len])
                        return (cmd, status_or_seq, payload)
                    buf.clear()

    def transact(
        self,
        cmd: int,
        seq: int = 0,
        payload: bytes = b"",
        timeout_s: float = 1.0,
        retries: int = 3,
    ) -> Tuple[int, bytes]:
        frame = build_frame(cmd, seq, payload)
        expect_cmd = cmd | RSP_MASK

        for attempt in range(retries):
            self.tr.write(frame)
            rsp = self._recv_frame(timeout_s=timeout_s)
            if rsp is not None:
                rcmd, status, rpl = rsp
                if rcmd == expect_cmd:
                    return (status, rpl)
            if self.verbose and attempt + 1 < retries:
                print(f"  [retry {attempt + 1}/{retries - 1}] cmd=0x{cmd:02X} seq={seq}")
        raise IAPError(f"命令 0x{cmd:02X} (seq={seq}) 通信超时（已重试 {retries} 次）")

    def query(self, timeout_s: float = 1.0, retries: int = 3) -> QueryInfo:
        status, pl = self.transact(CMD_QUERY, timeout_s=timeout_s, retries=retries)
        if status != STATUS_OK:
            raise IAPError(f"QUERY 失败: {STATUS_NAMES.get(status, status)}")
        return QueryInfo.from_payload(pl)

    def reboot(self) -> None:
        status, _ = self.transact(CMD_REBOOT, timeout_s=1.0, retries=2)
        if status != STATUS_OK:
            raise IAPError(f"REBOOT 失败: {STATUS_NAMES.get(status, status)}")

    def enter_bootloader(self, timeout_s: float = 3.0) -> QueryInfo:
        info = self.query()
        if info.run_mode == 0:
            return info

        if self.verbose:
            print(f"[*] 当前运行在 APP ({fmt_version(info.app_version)})，发送 CMD_REBOOT 切入 Bootloader...")
        self.reboot()
        time.sleep(0.03)
        self.tr.flush_rx()

        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                info = self.query(timeout_s=0.12, retries=1)
                if info.run_mode == 0:
                    return info
            except IAPError:
                pass
            time.sleep(0.02)
        raise IAPError("等待设备进入 Bootloader 超时")

    def start(self, version: int, size: int, crc32: int, resume: bool = True) -> int:
        flags = START_FLAG_RESUME if resume else 0
        pl = struct.pack("<IIIH", version, size, crc32, flags)
        status, rpl = self.transact(CMD_START, payload=pl, timeout_s=1.5, retries=3)
        if status != STATUS_OK or len(rpl) < 2:
            raise IAPError(f"START 失败: {STATUS_NAMES.get(status, status)}")
        return struct.unpack("<H", rpl[:2])[0]

    def send_data(self, seq: int, chunk: bytes, retries: int = 5) -> Tuple[int, int]:
        status, rpl = self.transact(CMD_DATA, seq=seq, payload=chunk, timeout_s=1.5, retries=retries)
        next_seq = struct.unpack("<H", rpl[:2])[0] if len(rpl) >= 2 else 0
        return status, next_seq

    def end(self) -> None:
        status, _ = self.transact(CMD_END, timeout_s=2.0, retries=2)
        if status != STATUS_OK:
            raise IAPError(f"END 校验失败: {STATUS_NAMES.get(status, status)}")

    def upgrade(
        self,
        fw_bytes: bytes,
        version: int,
        resume: bool = True,
        drop_after_pkts: int = 0,
        wait_boot_s: float = 0.2,
    ) -> Optional[QueryInfo]:
        # STM32F103 按半字(2B)编程，若镜像长度为奇数则补 1 字节 0xFF
        if len(fw_bytes) % 2 != 0:
            fw_bytes = fw_bytes + b"\xFF"
        total_size = len(fw_bytes)
        if total_size == 0 or total_size > 24 * 1024:
            raise IAPError(f"固件大小越界: {total_size} 字节（允许 2 ~ 24576）")

        fw_crc = crc32_ieee(fw_bytes)
        total_pkts = (total_size + PKT_CHUNK_SIZE - 1) // PKT_CHUNK_SIZE

        info = self.enter_bootloader()
        if self.verbose:
            print(f"[*] Bootloader 就绪: {info.pretty()}")
            print(
                f"[*] 开始升级 {fmt_version(version)} | size={total_size}B "
                f"({total_pkts} pkts) | crc32=0x{fw_crc:08X} | resume={resume}"
            )

        seq = self.start(version=version, size=total_size, crc32=fw_crc, resume=resume)
        if self.verbose:
            if seq > 0:
                print(f"[*] 命中断点续传！从 seq={seq} ({seq * PKT_CHUNK_SIZE}B) 继续发送")
            else:
                print("[*] 从 seq=0 开始全新传输")

        t0 = time.monotonic()
        sent_this_session = 0
        while seq < total_pkts:
            offset = seq * PKT_CHUNK_SIZE
            chunk = fw_bytes[offset : offset + PKT_CHUNK_SIZE]
            status, next_seq = self.send_data(seq, chunk)
            if status == STATUS_ERR_SEQ:
                if self.verbose:
                    print(f"  [!] 序号重同步: local={seq} -> remote_expect={next_seq}")
                seq = next_seq
                continue
            if status != STATUS_OK:
                raise IAPError(f"DATA seq={seq} 失败: {STATUS_NAMES.get(status, status)}")
            seq = next_seq
            sent_this_session += 1

            if self.verbose and (seq % 8 == 0 or seq == total_pkts):
                pct = seq * 100.0 / total_pkts
                print(f"    进度: {seq}/{total_pkts} ({pct:5.1f}%)")

            if drop_after_pkts > 0 and sent_this_session >= drop_after_pkts:
                if self.verbose:
                    print(f"[!] 故障注入：已发送 {sent_this_session} 包（当前 seq={seq}），主动模拟断链退出！")
                return None

        elapsed = max(0.001, time.monotonic() - t0)
        if self.verbose:
            print(f"[*] 数据发送完毕（耗时 {elapsed:.2f}s），发送 CMD_END 校验整包 CRC32...")
        self.end()

        if self.verbose:
            print("[*] CMD_END 通过！设备正在重启并执行 WAL 交换安装与试运行自检...")
        time.sleep(wait_boot_s)

        # 轮询查询安装后的最终运行状态
        deadline = time.monotonic() + 12.0
        while time.monotonic() < deadline:
            try:
                final_info = self.query(timeout_s=0.5, retries=1)
                if self.verbose:
                    print(f"[*] 升级后设备状态: {final_info.pretty()}")
                return final_info
            except IAPError:
                time.sleep(0.2)
        raise IAPError("升级后等待设备重新上线超时")


# ======================= SIL 测试固件生成器 =======================


def make_sim_firmware(version: int, size: int = 4096, deadlock: bool = False) -> bytes:
    """生成带合法 Cortex-M3 向量表 + SIL 行为标签(0x0100) 的测试固件镜像"""
    if size < 512 or (size % 2) != 0:
        raise ValueError("size must be >= 512 and even")
    buf = bytearray(size)
    seed = (version ^ (0x99 if deadlock else 0x11)) & 0xFF
    for i in range(size):
        buf[i] = ((i * 29) ^ (seed * 73) ^ (i >> 4)) & 0xFF
    # 合法向量表首两字：MSP=0x20005000, Reset_Handler=0x08002101
    struct.pack_into("<II", buf, 0, 0x20005000, 0x08002101)
    # 偏移 0x0100 写入 SIL 行为标签 ("SBAP", version, flags)
    flags = 1 if deadlock else 0
    struct.pack_into("<III", buf, 0x0100, 0x50414253, version, flags)
    return bytes(buf)


# ======================= CLI 入口 =======================


def open_transport(args: argparse.Namespace) -> BaseTransport:
    if args.sim:
        return SimPipeTransport(
            sim_exe=args.sim,
            flash_path=args.flash,
            boot_wait_ms=args.boot_wait_ms,
            stay_boot=args.stay_boot,
            verbose_sim=args.verbose_sim,
        )
    if args.port:
        return SerialTransport(port=args.port, baud=args.baud)
    raise SystemExit("请指定 --port COMx（真板）或 --sim safeboot_sim.exe（SIL 仿真器）")


def main() -> int:
    parser = argparse.ArgumentParser(description="SafeBoot-F1 上位机升级工具（串口 / SIL 双模）")
    parser.add_argument("--port", help="串口号，如 COM3")
    parser.add_argument("--baud", type=int, default=9600, help="串口波特率（默认 9600）")
    parser.add_argument("--sim", help="PC SIL 仿真器路径（safeboot_sim.exe）")
    parser.add_argument("--flash", help="SIL 持久化 Flash 镜像文件路径（如 sim_flash.bin）")
    parser.add_argument("--boot-wait-ms", type=int, default=200, help="SIL 握手窗口毫秒数")
    parser.add_argument("--stay-boot", action="store_true", help="SIL 启动时强制停在 Bootloader")
    parser.add_argument("--verbose-sim", action="store_true", help="打印 SIL stderr 调试日志")

    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("query", help="查询设备当前运行模式、版本、状态机与回滚事件")
    sub.add_parser("reboot", help="发送 CMD_REBOOT 软复位")

    p_up = sub.add_parser("upgrade", help="升级指定 .bin 固件")
    p_up.add_argument("bin_file", help="固件 .bin 文件路径")
    p_up.add_argument("--version", required=True, help="目标版本号，如 1.1 或 0x00010001")
    p_up.add_argument("--no-resume", action="store_true", help="禁用断点续传（从头重传）")
    p_up.add_argument("--drop-after", type=int, default=0, help="故障注入：发送 N 包后主动中断连接")
    p_up.add_argument("--wait-boot", type=float, default=1.5, help="CMD_END 后等待重启自检秒数")

    p_mk = sub.add_parser("mktestfw", help="生成 SIL 测试用固件 .bin")
    p_mk.add_argument("out_file", help="输出路径")
    p_mk.add_argument("--version", required=True, help="版本号，如 1.0 / 1.1 / 1.2")
    p_mk.add_argument("--size", type=int, default=4096, help="字节数（偶数，默认 4096）")
    p_mk.add_argument("--deadlock", action="store_true", help="注入死锁故障（触发 3 次看门狗自动回滚）")

    args = parser.parse_args()

    if args.cmd == "mktestfw":
        ver = parse_version(args.version)
        data = make_sim_firmware(ver, size=args.size, deadlock=args.deadlock)
        with open(args.out_file, "wb") as f:
            f.write(data)
        print(
            f"Generated {args.out_file}: {fmt_version(ver)} size={len(data)}B "
            f"crc32=0x{crc32_ieee(data):08X} deadlock={args.deadlock}"
        )
        return 0

    tr = open_transport(args)
    try:
        cli = SafeBootClient(tr, verbose=True)
        if args.cmd == "query":
            info = cli.query()
            print(info.pretty())
        elif args.cmd == "reboot":
            cli.reboot()
            print("REBOOT OK")
        elif args.cmd == "upgrade":
            with open(args.bin_file, "rb") as f:
                fw = f.read()
            ver = parse_version(args.version)
            wait_s = 0.15 if args.sim else args.wait_boot
            cli.upgrade(
                fw_bytes=fw,
                version=ver,
                resume=not args.no_resume,
                drop_after_pkts=args.drop_after,
                wait_boot_s=wait_s,
            )
    finally:
        tr.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
