# DS5Dongle BL616 移植

本目录是 DS5Dongle 的 BL616 单芯片实现，当前开发分支为
`dev_bl616`。

## 当前状态

- 芯片：BL616C50（BL616C，revision A0）
- 板级配置：`bl616dk`
- SDK：BouffaloSDK v2.3.30
- RTOS：FreeRTOS
- 蓝牙：Classic Bluetooth BR/EDR
- USB：CherryUSB，原生 DualSense HID + 全双工 UAC1
- Flash：4 MiB，固件分区从 `0x10000` 开始

手柄进入 `READY` 状态后，固件才会把 USB 设备连接到主机。正常
枚举名称为 `DualSense Wireless Controller`，VID/PID 为
`054C:0CE6`。未连接手柄时主机看不到该设备是当前的预期行为。

## 获取依赖

仓库使用固定版本的 SDK 和 Windows RISC-V 工具链子模块：

```powershell
git submodule update --init --recursive
```

## 协议契约测试

测试不需要连接硬件，但需要仓库内固定版本的 SDK：

```powershell
.\tools\host\python3.cmd -m unittest discover `
    -s .\ports\bl616\tests -p "test_*.py" -v
```

## 构建

BL616 固件必须通过仓库脚本构建，不要绕过脚本手工调用
`make`：

```powershell
.\tools\host\build_bl616.ps1 -Clean
```

产物位于 `ports/bl616/build/build_out/`：

- `ds5dongle_bl616_bl616.bin`：应用固件
- `boot2_bl616_isp_release_v8.1.8.bin`：Boot2
- `partition.bin`：4 MiB Flash 分区表
- `ds5dongle_bl616_bl616.elf` 和 `.map`：调试及尺寸分析

GitHub Actions 的 `BL616 CI` 工作流会在 BL616 相关提交和拉取请求
上运行相同的测试及 Clean 构建，并上传固件、Boot2、分区表和烧录配置。

## 烧录

1. 按住板载 BOOT 按钮，将 Type-C 线重新插入电脑。
2. 在设备管理器中确认 `USB\VID_349B&PID_6160` 对应的 COM 口。
   COM 号由 Windows 动态分配，不应写死。
3. 在 `ports/bl616` 目录运行：

```powershell
$FlashPort = "COM4" # 改为设备管理器中实际显示的端口

..\..\third_party\bouffalo_sdk\tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe `
    --port $FlashPort --baudrate 2000000 --chipname bl616 `
    --config flash_prog_cfg.ini write_flash_files
```

出现 `Flash writing succeeded` 和 `Verification succeeded` 表示写入
及设备端 SHA256 回读校验成功。随后拔线，不按 BOOT 重新上电。

## 配对与运行

首次使用时，按住 DualSense 的 PS + Create/Share 键进入蓝牙配对模式，
再给适配器上电。已保存的手柄会在后续启动时自动重连。当前实现只维护
一个活动手柄连接。

运行态 USB 描述符只有 HID 和 Audio，不提供 CDC ACM 串口。BOOT 模式
出现的 COM 口属于 BL616 BootROM 下载接口，与运行态固件无关。

## 实时诊断

手柄连接且 USB 已正常枚举后，可在 Windows 仓库根目录读取 HID
Feature `0xF0`（USB/音频流水线）和 `0xF1`（Bluetooth/HCI 传输）
快照：

```powershell
.\tools\host\python3.cmd .\tools\host\read_bl616_diagnostics.py
.\tools\host\python3.cmd .\tools\host\read_bl616_diagnostics.py --json
```

诊断包含 USB 丢包、Opus 编码耗时、邮箱丢帧、HCI 完成延迟以及主要
实时任务的栈高水位。首次 USB 激活后，UART 日志还会输出
`app_start` 任务的最小剩余栈空间，供后续安全收紧其 4 KiB 静态栈。

## 已知限制

- 构建和烧录链目前依赖仓库内 Windows 工具。
- CI 只能验证协议契约和固件构建，蓝牙配对、USB 枚举及音频仍需实机
  冒烟测试。
- `bl616dk` 与当前 4 MiB 板卡已经过构建、烧录及运行验证，但调试
  UART、蓝色 LED 和扩展排针的实际引脚仍未确认。
- USB 只有在手柄连接完成后才出现；这不是线材或驱动故障。
- 不需要为了本适配器手动更改系统音频输出设备。游戏会像 PS5 直连
  手柄时一样，把对应音效发送到手柄扬声器。

板级实测记录见 [`docs/HARDWARE_BL616.md`](../../docs/HARDWARE_BL616.md)。
