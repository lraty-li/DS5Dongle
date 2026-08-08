# AGENTS.md — DS5Dongle 开发约束

## 项目概览

- `master`：RP2350（Pico 双核）实现（`src/`、`boards/`），当前主线
- `dev_bl616`：BL616 单芯片移植（`ports/bl616/`），当前开发分支

## 硬性约束：BL616 固件构建

**必须使用仓库编译脚本 `tools\host\build_bl616.ps1`，禁止绕过脚本手工执行 make。**

```powershell
# 完整重建（推荐，-Clean 会先清理旧产物）
.\tools\host\build_bl616.ps1 -Clean
```e tools\host\env.ps1` 加载同一环境。

### 构建产物

`ports/bl616/build/build_out/`：

- `ds5dongle_bl616_bl616.bin` — 固件
- `boot2_bl616_isp_release_v8.1.8.bin` — 启动引导
- `partition.bin` — 分区表（由 `partition_cfg_4M.toml` 生成，FW @0x10000）
- `mfg_*.bin` — RF 校准镜像（烧录配置未使用，板子出厂已烧）

## 烧录（BL616）

1. 按住 BOOT 插入 USB，Windows 出现下载口 `USB\VID_349B&PID_6160`（通常 COM4）
2. 烧录（在 `ports/bl616` 目录下执行）：

```powershell
..\..\third_party\bouffalo_sdk\tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe `
  --port COM4 --baudrate 2000000 --chipname bl616 `
  --config flash_prog_cfg.ini write_flash_files
```

3. 烧录成功标志：`Flash writing succeeded` + `Verification succeeded`（设备端回读 SHA256 一致）
4. 拔线（不按 BOOT）重新上电，运行固件

## 测试（BL616 协议契约测试，无需硬件）

```powershell
.\tools\host\python3.cmd -m unittest discover -s .\ports\bl616\tests -p "test_*.py" -v
```

## 已知注意事项

- `docs/HARDWARE_BL616.md`：板级硬件记录（USB 布线、引脚均以实测为准）
- 板子正常上电后应枚举为 `DualSense Wireless Controller`；若未识别，先确认
  固件是否真正运行（下载口 COM4 消失 = 固件在跑）
用 USB 连接手柄时**没有更改过音频输出设备**，
  任何问题都与音频输出设备设置无关（PS5 直连手柄时自动路由扬声器）。游戏会直接将音效输出到手柄扬声器


# DS5Dongle BL616 Port

## Target

- Chip: BL616
- Board configuration: BL616DK
- SDK: BouffaloSDK v2.3.30
- RTOS: FreeRTOS
- Bluetooth: Classic Bluetooth BR/EDR
- USB stack: CherryUSB

## Protocol contract tests

The platform-independent DualSense wire-format tests use the repository Python
entry point and do not require the target board:

```powershell
.\tools\host\python3.cmd -m unittest discover `
    -s .\ports\bl616\tests -p "test_*.py" -v
```

