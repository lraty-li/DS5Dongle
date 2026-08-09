#!/usr/bin/env python3
"""Read the BL616 real-time pipeline diagnostics over HID Feature reports."""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import math
import struct
import sys


VID = 0x054C
PID = 0x0CE6
REPORT_SIZE = 64
PIPELINE_REPORT_ID = 0xF0
TRANSPORT_REPORT_ID = 0xF1
DIAGNOSTIC_VERSION = 1


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wintypes.ULONG),
        ("VendorID", wintypes.USHORT),
        ("ProductID", wintypes.USHORT),
        ("VersionNumber", wintypes.USHORT),
    ]


def _u16(data: bytes, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def decode_pipeline(data: bytes) -> dict[str, int | bool]:
    if len(data) != REPORT_SIZE or data[0] != PIPELINE_REPORT_ID:
        raise ValueError("流水线诊断报告长度或 Report ID 不正确")
    if data[1:5] != b"D5D0" or data[5] != DIAGNOSTIC_VERSION:
        raise ValueError("固件未返回受支持的流水线诊断格式")

    flags = data[6]
    return {
        "speaker_stream_open": bool(flags & 0x01),
        "microphone_stream_open": bool(flags & 0x02),
        "codec_ready": bool(flags & 0x04),
        "encode_overrun_seen": bool(flags & 0x08),
        "usb_gap_seen": bool(flags & 0x10),
        "encode_overruns": data[7],
        "usb_packets": _u32(data, 8),
        "usb_invalid": _u32(data, 12),
        "usb_dropped": _u32(data, 16),
        "usb_arm_failures": _u32(data, 20),
        "usb_gaps_over_1500us": _u32(data, 24),
        "usb_max_interval_us": _u32(data, 28),
        "haptics_published": _u32(data, 32),
        "haptics_mailbox_dropped": _u32(data, 36),
        "raw_speaker_frames_dropped": _u32(data, 40),
        "opus_frames_published": _u32(data, 44),
        "opus_mailbox_dropped": _u32(data, 48),
        "encode_count": _u32(data, 52),
        "encode_average_us": _u16(data, 56),
        "encode_max_us": _u16(data, 58),
        "audio_task_stack_free_bytes": _u16(data, 60) * 4,
        "codec_task_stack_free_bytes": _u16(data, 62) * 4,
    }


def decode_transport(data: bytes) -> dict[str, int | bool | str]:
    if len(data) != REPORT_SIZE or data[0] != TRANSPORT_REPORT_ID:
        raise ValueError("传输诊断报告长度或 Report ID 不正确")
    if data[1:5] != b"D5D1" or data[5] != DIAGNOSTIC_VERSION:
        raise ValueError("固件未返回受支持的传输诊断格式")

    state_number = data[6]
    state_names = {
        0: "off",
        1: "discovering",
        2: "idle",
        3: "candidate-ready",
        4: "reconnect-wait",
        5: "acl-connecting",
        6: "securing",
        7: "l2cap-connecting",
        8: "ready",
        9: "disconnecting",
    }
    flags = data[7]
    mtu = _u16(data, 8)
    return {
        "bt_state_number": state_number,
        "bt_state": state_names.get(state_number, f"unknown-{state_number}"),
        "control_channel_ready": bool(flags & 0x01),
        "interrupt_channel_ready": bool(flags & 0x02),
        "hci_acl_mtu": mtu,
        "estimated_acl_fragments_per_audio_report": (
            math.ceil((548 + 4) / mtu) if mtu else 0
        ),
        "hci_free_packets_at_last_submit": data[10],
        "hci_max_free_packets_seen": data[11],
        "hci_min_free_packets_seen": data[12],
        "host_tx_queue_at_last_submit": data[13],
        "host_tx_queue_max": data[14],
        "completion_sample_slots_busy": data[15],
        "audio_send_attempts": _u32(data, 16),
        "audio_send_accepted": _u32(data, 20),
        "audio_send_immediate_failures": _u32(data, 24),
        "audio_send_enobufs": _u32(data, 28),
        "audio_submit_max_interval_us": _u32(data, 32),
        "hci_zero_slot_observations": _u32(data, 36),
        "completion_samples_submitted": _u32(data, 40),
        "completion_samples_completed": _u32(data, 44),
        "completion_last_us": _u32(data, 48),
        "completion_max_us": _u32(data, 52),
        "completion_average_us": _u32(data, 56),
        "bt_tx_task_stack_free_bytes": _u16(data, 60) * 4,
        "bt_rx_task_stack_free_bytes": _u16(data, 62) * 4,
    }


def _configure_windows_apis():
    if sys.platform != "win32":
        raise RuntimeError("该读取工具目前只支持 Windows")

    hid = ctypes.WinDLL("hid.dll", use_last_error=True)
    cfgmgr32 = ctypes.WinDLL("cfgmgr32.dll", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32.dll", use_last_error=True)

    hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
    hid.HidD_GetHidGuid.restype = None
    hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE,
                                       ctypes.POINTER(HIDD_ATTRIBUTES)]
    hid.HidD_GetAttributes.restype = wintypes.BOOLEAN
    hid.HidD_GetFeature.argtypes = [wintypes.HANDLE, ctypes.c_void_p,
                                    wintypes.ULONG]
    hid.HidD_GetFeature.restype = wintypes.BOOLEAN

    cfgmgr32.CM_Get_Device_Interface_List_SizeW.argtypes = [
        ctypes.POINTER(wintypes.ULONG), ctypes.POINTER(GUID),
        wintypes.LPCWSTR, wintypes.ULONG,
    ]
    cfgmgr32.CM_Get_Device_Interface_List_SizeW.restype = wintypes.ULONG
    cfgmgr32.CM_Get_Device_Interface_ListW.argtypes = [
        ctypes.POINTER(GUID), wintypes.LPCWSTR, ctypes.c_void_p,
        wintypes.ULONG, wintypes.ULONG,
    ]
    cfgmgr32.CM_Get_Device_Interface_ListW.restype = wintypes.ULONG

    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    return hid, cfgmgr32, kernel32


def _hid_paths(hid, cfgmgr32) -> list[str]:
    hid_guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(hid_guid))
    character_count = wintypes.ULONG()
    result = cfgmgr32.CM_Get_Device_Interface_List_SizeW(
        ctypes.byref(character_count), ctypes.byref(hid_guid), None, 0
    )
    if result != 0:
        raise OSError(f"枚举 HID 接口长度失败，CONFIGRET={result}")

    buffer = (ctypes.c_wchar * character_count.value)()
    result = cfgmgr32.CM_Get_Device_Interface_ListW(
        ctypes.byref(hid_guid), None, ctypes.byref(buffer),
        character_count.value, 0
    )
    if result != 0:
        raise OSError(f"枚举 HID 接口失败，CONFIGRET={result}")
    return [path for path in "".join(buffer).split("\0") if path]


def _open_dongle_hid():
    hid, cfgmgr32, kernel32 = _configure_windows_apis()
    generic_read = 0x80000000
    generic_write = 0x40000000
    share_read_write = 0x00000003
    open_existing = 3
    invalid_handle = ctypes.c_void_p(-1).value
    candidates = []

    for path in _hid_paths(hid, cfgmgr32):
        lowered = path.lower()
        if "vid_054c" not in lowered or "pid_0ce6" not in lowered:
            continue
        candidates.append(path)
        handle = kernel32.CreateFileW(
            path, generic_read | generic_write, share_read_write, None,
            open_existing, 0, None
        )
        if handle in (None, invalid_handle):
            continue

        attributes = HIDD_ATTRIBUTES()
        attributes.Size = ctypes.sizeof(attributes)
        if (hid.HidD_GetAttributes(handle, ctypes.byref(attributes)) and
                attributes.VendorID == VID and attributes.ProductID == PID):
            return hid, kernel32, handle, path
        kernel32.CloseHandle(handle)

    if not candidates:
        raise FileNotFoundError(
            "未找到 VID_054C&PID_0CE6 的 HID 接口；请确认 dongle 正常枚举"
        )
    raise PermissionError(
        "找到了 DualSense HID 接口，但无法以共享读写方式打开"
    )


def _get_feature(hid, handle, report_id: int) -> bytes:
    report = (ctypes.c_ubyte * REPORT_SIZE)()
    report[0] = report_id
    if not hid.HidD_GetFeature(handle, report, REPORT_SIZE):
        error = ctypes.get_last_error()
        raise OSError(
            error,
            f"读取 Feature 0x{report_id:02X} 失败；请确认已烧录诊断固件",
        )
    return bytes(report)


def read_diagnostics() -> tuple[str, dict, dict]:
    hid, kernel32, handle, path = _open_dongle_hid()
    try:
        pipeline = decode_pipeline(
            _get_feature(hid, handle, PIPELINE_REPORT_ID)
        )
        transport = decode_transport(
            _get_feature(hid, handle, TRANSPORT_REPORT_ID)
        )
    finally:
        kernel32.CloseHandle(handle)
    return path, pipeline, transport


def _print_human(path: str, pipeline: dict, transport: dict) -> None:
    print(f"HID: {path}")
    print("\nUSB / 编码流水线")
    for key, value in pipeline.items():
        print(f"  {key}: {value}")
    print("\nBluetooth / HCI 传输")
    for key, value in transport.items():
        print(f"  {key}: {value}")

    findings = []
    if pipeline["usb_dropped"] or pipeline["usb_arm_failures"]:
        findings.append("USB 接收路径存在明确丢包或重新挂载失败。")
    if (pipeline["raw_speaker_frames_dropped"] or
            pipeline["opus_mailbox_dropped"] or
            pipeline["encode_max_us"] >= 11000):
        findings.append("Opus/调度路径未持续满足 10.67 ms 实时预算。")
    if transport["audio_send_immediate_failures"]:
        findings.append("L2CAP 在进入控制器前已经丢弃音频报告。")
    if (transport["hci_zero_slot_observations"] or
            transport["completion_max_us"] >= 30000):
        findings.append("HCI/BR-EDR 控制器存在排队或空口发送延迟。")
    submitted = transport["completion_samples_submitted"]
    completed = transport["completion_samples_completed"]
    if submitted and completed < submitted:
        findings.append("仍有抽样音频报告未收到控制器完成确认。")

    print("\n自动判读")
    if findings:
        for finding in findings:
            print(f"  - {finding}")
    else:
        print("  - 快照未显示显式丢帧；需结合计数比例和完成时延进一步判断。")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="读取 DS5Dongle BL616 实时音频/蓝牙诊断快照"
    )
    parser.add_argument("--json", action="store_true", help="输出 JSON")
    args = parser.parse_args()

    try:
        path, pipeline, transport = read_diagnostics()
    except (OSError, ValueError, RuntimeError) as error:
        print(f"错误：{error}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps({"hid_path": path, "pipeline": pipeline,
                          "transport": transport}, ensure_ascii=False,
                         indent=2))
    else:
        _print_human(path, pipeline, transport)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
