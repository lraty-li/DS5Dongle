# DS5Dongle BL616 Port

## Target

- Chip: BL616
- Board configuration: BL616DK
- SDK: BouffaloSDK v2.3.30
- RTOS: FreeRTOS
- Bluetooth: Classic Bluetooth BR/EDR
- USB stack: CherryUSB

## First-version scope

- DualSense Bluetooth HID over L2CAP
- USB HID device
- Input report forwarding
- Output report forwarding
- No audio support

All project-specific BL616 source code must remain under this directory.
Do not modify or add project code inside `third_party/bouffalo_sdk`.

## Protocol contract tests

The platform-independent DualSense wire-format tests use the repository Python
entry point and do not require the target board:

```powershell
.\tools\host\python3.cmd -m unittest discover `
    -s .\ports\bl616\tests -p "test_*.py" -v
```

These cover the wire format, the BR/EDR discovery candidate policy, and source
contracts for the connection/security state machine. The BL616 clean build
compiles the corresponding C implementations with the repository-pinned
RISC-V toolchain.
